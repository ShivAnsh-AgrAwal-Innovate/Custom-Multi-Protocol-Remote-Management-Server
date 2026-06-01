#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <endian.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define OP_PING 0x01
#define OP_AUTH_REQ 0x02
#define OP_CMD_REQ 0x03
#define OP_FILE_INIT 0x04
#define OP_SYS_MONITOR 0x05
#define OP_SHELL_EXEC 0x06
#define OP_FILE_TRANSFER 0x07

#define RES_SUCCESS 0x101
#define RES_AUTH_OK 0x102
#define RES_ERR_FAIL 0x103
#define RES_ERR_DENIED 0x104

#define RES_STREAM_DATA 0x201
#define RES_STREAM_END 0x202

#define FT_INIT 0x301
#define FT_INIT_ACK 0x302
#define FT_DATA 0x303
#define FT_DATA_ACK 0x304
#define FT_COMP 0x305
#define FT_ERR 0x306

#define MAX_CHUNK_CEILING 65536

struct messageHeader
{
    uint32_t messageLength;
    uint32_t messageType;
};

struct fileInitPayload
{
    uint64_t fileSize;
    char fileName[256];
};

struct fileChunkHeader
{
    uint32_t sequenceNumber;
    uint32_t chunkDataLength;
};

ssize_t read_exact(int fd, void *buf, size_t total_bytes)
{
    size_t bytes_read = 0;
    char *ptr = (char *)buf;
    while (bytes_read < total_bytes)
    {
        ssize_t n = recv(fd, ptr + bytes_read, total_bytes - bytes_read, 0);
        if (n < 0)
        {
            return -1;
        }
        if (n == 0)
        {
            return 0;
        }
        bytes_read += n;
    }
    return bytes_read;
}

void executeBinaryUpload(int socketFd, const char *localFilePath)
{

    FILE *file = fopen(localFilePath, "rb");
    if (file == NULL)
    {
        printf("Error: Failed to open '%s'!\n", localFilePath);
        return;
    }

    fseek(file, 0, SEEK_END);
    uint64_t totalFileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    printf("Calculate file size: %lu bytes\n", totalFileSize);

    struct messageHeader outboundHeader;
    outboundHeader.messageType = htonl(FT_INIT);
    outboundHeader.messageLength = htonl(sizeof(struct fileInitPayload));

    struct fileInitPayload initPayload;
    initPayload.fileSize = htobe64(totalFileSize);

    const char *justFileName = strrchr(localFilePath, '/');
    if (justFileName == NULL)
    {
        justFileName = localFilePath;
    }
    else
    {
        justFileName++;
    }

    memset(initPayload.fileName, 0, sizeof(initPayload.fileName));
    strncpy(initPayload.fileName, justFileName, 255);

    send(socketFd, &outboundHeader, sizeof(outboundHeader), 0);
    send(socketFd, &initPayload, sizeof(initPayload), 0);

    struct messageHeader initAckHeader;
    if (read_exact(socketFd, &initAckHeader, sizeof(initAckHeader)) <= 0 || ntohl(initAckHeader.messageType) != FT_INIT_ACK)
    {
        printf("Error: Server terminated file transfer!\n");
        fclose(file);
        return;
    }

    char chunkDataBuffer[4096];
    uint32_t sequenceCounter = 0;
    size_t actualBytesRead;

    printf("Starting data transmission...\n");

    while ((actualBytesRead = fread(chunkDataBuffer, 1, sizeof(chunkDataBuffer), file)) > 0)
    {
        int ackVerified = 0;
        int transmissionRetries = 0;
        while (!ackVerified)
        {
            struct messageHeader dataHeader;
            dataHeader.messageType = htonl(FT_DATA);
            dataHeader.messageLength = htonl(sizeof(struct fileChunkHeader) + actualBytesRead);

            struct fileChunkHeader chunkHeader;
            chunkHeader.sequenceNumber = htonl(sequenceCounter);
            chunkHeader.chunkDataLength = htonl((uint32_t)actualBytesRead);

            send(socketFd, &dataHeader, sizeof(dataHeader), 0);
            send(socketFd, &chunkHeader, sizeof(chunkHeader), 0);
            send(socketFd, chunkDataBuffer, actualBytesRead, 0);

            struct messageHeader chunkAckHeader;
            if (read_exact(socketFd, &chunkAckHeader, sizeof(chunkAckHeader)) > 0)
            {
                uint32_t resType = ntohl(chunkAckHeader.messageType);
                if (resType == FT_DATA_ACK)
                {
                    uint32_t serverConfirmedSeq;
                    read_exact(socketFd, &serverConfirmedSeq, sizeof(serverConfirmedSeq));
                    serverConfirmedSeq = ntohl(serverConfirmedSeq);

                    if (serverConfirmedSeq == sequenceCounter)
                    {
                        ackVerified = 1;
                    }
                }
            }

            if (!ackVerified)
            {
                transmissionRetries++;
                printf("ACK chunk for #%u missing. Trying again (%d / 5)\n", sequenceCounter, transmissionRetries);

                if (transmissionRetries > 5)
                {
                    printf("File transfer unsuccessful. Aborting file transfer...\n");
                    fclose(file);
                    return;
                }
                usleep(300000);
            }
        }
        sequenceCounter++;
    }

    struct messageHeader completionHeader;
    completionHeader.messageType = htonl(FT_COMP);
    completionHeader.messageLength = htonl(0);
    send(socketFd, &completionHeader, sizeof(completionHeader), 0);

    printf("File transfer successful!\n");
    fclose(file);
    return;
}

void executeBinaryDownload(int socketFd, const char *localSavePath)
{
    struct fileInitPayload incomingPayloadHeader;
    if (read_exact(socketFd, &incomingPayloadHeader, sizeof(incomingPayloadHeader)) <= 0)
    {
        printf("Error: Failed to read payload header from the server.\n");
        return;
    }

    uint64_t totalIncomingBytes = be64toh(incomingPayloadHeader.fileSize);
    printf("Target File Name: %s (%lu bytes)\n", incomingPayloadHeader.fileName, totalIncomingBytes);
    printf("Saving file locally to: %s\n", localSavePath);

    FILE *destFile = fopen(localSavePath, "wb");
    if (destFile == NULL)
    {
        printf("Error: Failed to create a local file path '%s'!\n", localSavePath);
        struct messageHeader errHeader;
        errHeader.messageLength = htonl(0);
        errHeader.messageType = htonl(FT_ERR);
        send(socketFd, &errHeader, sizeof(errHeader), 0);
        return;
    }

    struct messageHeader outboundAckHeader;
    outboundAckHeader.messageLength = htonl(0);
    outboundAckHeader.messageType = htonl(FT_INIT_ACK);
    send(socketFd, &outboundAckHeader, sizeof(outboundAckHeader), 0);

    uint32_t expectedSequence = 0;
    printf("Downloading file...\n");

    while (1)
    {
        struct messageHeader chunkInfoHeader;
        if (read_exact(socketFd, &chunkInfoHeader, sizeof(chunkInfoHeader)) <= 0)
        {
            printf("Error: Connection lost with server mid-stream\n");
            fclose(destFile);
            return;
        }

        uint32_t frameType = ntohl(chunkInfoHeader.messageType);

        if (frameType == FT_DATA)
        {
            struct fileChunkHeader chunkHeader;
            read_exact(socketFd, &chunkHeader, sizeof(chunkHeader));
            uint32_t blockSequence = ntohl(chunkHeader.sequenceNumber);
            uint32_t rawDataLength = ntohl(chunkHeader.chunkDataLength);

            if (rawDataLength > MAX_CHUNK_CEILING)
            {
                fclose(destFile); 
                return;
            }

            char *payloadBuffer = (char *)malloc(rawDataLength);
            if (payloadBuffer == NULL)
            {
                perror("Malloc Allocation Failed!");
                continue;
            }

            read_exact(socketFd, payloadBuffer, rawDataLength);

            if (blockSequence == expectedSequence)
            {
                fwrite(payloadBuffer, 1, rawDataLength, destFile);
                expectedSequence++;
            }
            else
            {
                printf("Warning: Mismatch in Sequence numbers. Got #%u Expected #%u!\n", blockSequence, expectedSequence);
            }
            free(payloadBuffer);

            struct messageHeader outboundDataAckHeader;
            outboundDataAckHeader.messageType = htonl(FT_DATA_ACK);
            outboundDataAckHeader.messageLength = htonl(sizeof(uint32_t));

            uint32_t outgoingAckSequence = htonl(expectedSequence - 1);
            send(socketFd, &outboundDataAckHeader, sizeof(outboundDataAckHeader), 0);
            send(socketFd, &outgoingAckSequence, sizeof(outgoingAckSequence), 0);
        }
        else if (frameType == FT_COMP)
        {
            fclose(destFile);
            printf("Successful file transfer!");
            break;
        }
        else if (frameType == FT_ERR)
        {
            printf("Error: Server reported file read exception!");
            fclose(destFile);
            return;
        }
    }
}

int main(void)
{
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd < 0)
    {
        perror("Socket creation failed!");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(clientFd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connect failed!");
        close(clientFd);
        return -1;
    }

    printf("Connected!\nType your message and hit 'Enter'. Type 'exit' to quit.\n");
    while (1)
    {
        char clientInput[1024] = {0};

        printf("\nYou > ");
        if (fgets(clientInput, sizeof(clientInput), stdin) == NULL)
        {
            break;
        }

        for (int i = 0; clientInput[i] != '\0'; i++)
        {
            if (clientInput[i] == '\n')
            {
                clientInput[i] = '\0';
                break;
            }
        }

        uint32_t inputLength = strlen(clientInput);
        if (inputLength == 0)
        {
            continue;
        }

        char tempClientInput[1024] = {0};
        strcpy(tempClientInput, clientInput);
        char *keyword = strtok(tempClientInput, " ");

        if (keyword == NULL)
        {
            continue;
        }

        uint32_t opcode = 0;
        char payloadBuffer[1024] = {0};

        if (strcasecmp(keyword, "EXIT") == 0)
        {
            break;
        }
        else if (strcasecmp(keyword, "PING") == 0)
        {
            opcode = OP_PING;
        }
        else if (strcasecmp(keyword, "AUTH") == 0)
        {
            opcode = OP_AUTH_REQ;
            char *args = clientInput + strlen(keyword);
            while (*args == ' ')
            {
                args++;
            }
            strcpy(payloadBuffer, args);
        }
        else if (strcasecmp(keyword, "COMMAND") == 0)
        {
            opcode = OP_CMD_REQ;
            char *args = clientInput + strlen(keyword);
            while (*args == ' ')
            {
                args++;
            }
            strcpy(payloadBuffer, args);
        }
        else if (strcasecmp(keyword, "FILE_INIT") == 0)
        {
            opcode = OP_FILE_INIT;
        }
        else if (strcasecmp(keyword, "MONITOR") == 0)
        {
            opcode = OP_SYS_MONITOR;
        }
        else if (strcasecmp(keyword, "EXEC") == 0)
        {
            opcode = OP_SHELL_EXEC;
            char *args = clientInput + strlen(keyword);
            while (*args == ' ')
            {
                args++;
            }
            strcpy(payloadBuffer, args);
        }
        else if (strcasecmp(keyword, "UPLOAD") == 0)
        {
            opcode = OP_FILE_TRANSFER;
            char *localPath = strtok(NULL, " ");
            char *remoteName = strtok(NULL, " ");

            if (localPath == NULL || remoteName == NULL)
            {
                printf("Usage: UPLOAD [local_file_path] [remote_file_name]\n");
                continue;
            }

            FILE *checkFile = fopen(localPath, "rb");
            if (checkFile == NULL)
            {
                printf("Error: Local file '%s' does not exits!\n", localPath);
                continue;
            }
            fclose(checkFile);

            memset(payloadBuffer, 0, sizeof(payloadBuffer));
            snprintf(payloadBuffer, sizeof(payloadBuffer), "UP:%s:%s", localPath, remoteName);
        }
        else if (strcasecmp(keyword, "DOWNLOAD") == 0)
        {
            opcode = OP_FILE_TRANSFER;
            char *localPath = strtok(NULL, " ");
            char *remoteName = strtok(NULL, " ");

            if (localPath == NULL || remoteName == NULL)
            {
                printf("Usage: DOWNLOAD [local_file_path] [remote_file_name]\n");
                continue;
            }

            memset(payloadBuffer, 0, sizeof(payloadBuffer));
            snprintf(payloadBuffer, sizeof(payloadBuffer), "DOWN:%s:%s", localPath, remoteName);
        }
        else
        {
            printf("Unknown Instruction!\n");
            continue;
        }

        struct messageHeader outboundHeader;
        uint32_t payloadLength = strlen(payloadBuffer);
        outboundHeader.messageLength = htonl(payloadLength);
        outboundHeader.messageType = htonl(opcode);

        send(clientFd, &outboundHeader, sizeof(outboundHeader), 0);
        if (payloadLength > 0)
        {
            send(clientFd, payloadBuffer, payloadLength, 0);
        }
        while (1)
        {

            struct messageHeader incomingHeader;
            int headerBytes = read_exact(clientFd, &incomingHeader, sizeof(incomingHeader));
            if (headerBytes == 0)
            {
                printf("Server Disconnected during header read!\n");
                break;
            }
            if (headerBytes < 0)
            {
                perror("Read error!");
                break;
            }

            uint32_t resLength = ntohl(incomingHeader.messageLength);
            uint32_t resType = ntohl(incomingHeader.messageType);

            if (resType == RES_STREAM_END)
            {
                printf("\nCommand Execution Complete!\n");
                break;
            }

            char buffer[1024] = {0};
            if (resLength >= 1024)
            {
                printf("Buffer overload!\n");
                break;
            }

            if (resLength > 0)
            {
                int bodyBytes = read_exact(clientFd, buffer, resLength);
                if (bodyBytes <= 0)
                {
                    printf("Server Disconnected during body payload read!\n");
                    break;
                }
            }
            if (resType == RES_STREAM_DATA)
            {
                printf("%s", buffer);
                fflush(stdout);
            }
            else if (resType == FT_INIT_ACK)
            {
                printf("Server granted disk access. Initializing upload stream...\n");
                char tempPaths[1024];
                strcpy(tempPaths, payloadBuffer);
                strtok(tempPaths, ":");
                char *localFilePath = strtok(NULL, ":");

                executeBinaryUpload(clientFd, localFilePath);
                break;
            }
            else if (resType == FT_INIT)
            {
                printf("Server granted downloading permissions. Incoming file stream activated...\n");
                char tempPaths[1024];
                strcpy(tempPaths, payloadBuffer);

                strtok(tempPaths, ":");
                strtok(NULL, ":");
                char *localSaveName = strtok(NULL, ":");

                if (localSaveName == NULL)
                {
                    localSaveName = "downloaded.bin";
                }

                executeBinaryDownload(clientFd, localSaveName);
                break;
            }
            else
            {
                if (resType == RES_SUCCESS)
                {
                    printf("SUCCESS: ");
                }
                else if (resType == RES_AUTH_OK)
                {
                    printf("AUTH_OK: ");
                }
                else if (resType == RES_ERR_FAIL)
                {
                    printf("EXEC_ERR: ");
                }
                else if (resType == RES_ERR_DENIED)
                {
                    printf("ACCESS DENIED: ");
                }
                else
                {
                    printf("UNKNOWN RESPONSE CODE: ");
                }

                if (resLength > 0)
                {
                    printf("%s\n\n", buffer);
                }
                else
                {
                    printf("(Empty Body)\n\n");
                }
                break;
            }
        }
    }

    close(clientFd);
    return 0;
}