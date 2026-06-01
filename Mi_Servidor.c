#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <endian.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

#define MAX_PORTS 3
#define MAX_EVENTS 64
#define MAX_CHUNK_CEILING 65536

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

struct messageHeader
{
    uint32_t messageLength;
    uint32_t messageType;
};

typedef struct
{
    int fd;
    int is_authenticated;
    uint16_t connected_port;
} ClientState;

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

int securePorts[MAX_PORTS] = {8080, 9000, 9999};
int serverFds[MAX_PORTS] = {-1, -1, -1};
int on = 1;

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

void pingHandler(ClientState *client)
{
    printf("[Handler 0x01] Processing Ping from FD #%d --- \n", client->fd);

    char *replyText = "PONG";
    uint32_t replyLength = strlen(replyText);

    struct messageHeader outboundHeader;
    outboundHeader.messageLength = htonl(replyLength);
    outboundHeader.messageType = htonl(RES_SUCCESS);

    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, replyText, replyLength, 0);
}

void authorisationHandler(ClientState *client, char *buffer, uint32_t messageLength)
{
    printf("[Handler 0x02] Processing Authorisation Request from FD #%d --- \n", client->fd);

    struct messageHeader outboundHeader;
    char *replyText;

    if (messageLength > 0 && strcmp(buffer, "Work ^v^") == 0)
    {
        printf("Success! Password verified for FD #%d\n", client->fd);
        replyText = "ACCESS_GRANTED";
        outboundHeader.messageType = htonl(RES_AUTH_OK);
        client->is_authenticated = 1;
    }
    else
    {
        printf("Denied! Invalid credentials from FD #%d\n", client->fd);
        replyText = "INVALID_CREDENTIALS";
        outboundHeader.messageType = htonl(RES_ERR_DENIED);
    }

    uint32_t replyLength = strlen(replyText);
    outboundHeader.messageLength = htonl(replyLength);
    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, replyText, replyLength, 0);
}

void commandHandler(ClientState *client, char *buffer)
{
    printf("[Handler 0x03] Executing command from FD #%d: '%s' --- \n", client->fd, buffer);

    if (client->is_authenticated == 0)
    {
        printf("Denied! Unauthenticated command attempt on FD #%d\n", client->fd);
        struct messageHeader outboundHeader;
        char *replyText = "ERROR: AUTHENTICATION_REQUIRED";
        uint32_t replyLength = strlen(replyText);
        outboundHeader.messageLength = htonl(replyLength);
        outboundHeader.messageType = htonl(RES_ERR_DENIED);
        send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
        send(client->fd, replyText, replyLength, 0);
        return;
    }

    char replyText[1024];
    struct messageHeader outboundHeader;

    if (strcmp(buffer, "GET_STATUS") == 0)
    {
        snprintf(replyText, sizeof(replyText), "SERVER STATUS: OPERATIONAL | PORT: %d\n", client->connected_port);
        outboundHeader.messageType = htonl(RES_SUCCESS);
    }
    else
    {
        snprintf(replyText, sizeof(replyText), "ERROR: UNKNOWN_COMMAND '%s'", buffer);
        outboundHeader.messageType = htonl(RES_ERR_FAIL);
    }

    uint32_t replyLength = strlen(replyText);
    outboundHeader.messageLength = htonl(replyLength);
    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, replyText, replyLength, 0);
}

void fileTransferInitialisationHandler(ClientState *client)
{
    printf("[Handler 0x04] Allocation file stream channel for FD #%d --- \n", client->fd);

    if (client->is_authenticated == 0)
    {
        struct messageHeader outboundHeader;
        char *replyText = "ERROR: AUTHENTICATION_REQUIRED";
        uint32_t replyLength = strlen(replyText);
        outboundHeader.messageLength = htonl(replyLength);
        outboundHeader.messageType = htonl(RES_ERR_DENIED);
        send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
        send(client->fd, replyText, replyLength, 0);
        return;
    }

    char *replyText = "STREAM_INITIALISED_SUCCESSFULLY";
    uint32_t replyLength = strlen(replyText);

    struct messageHeader outboundHeader;
    outboundHeader.messageLength = htonl(replyLength);
    outboundHeader.messageType = htonl(RES_SUCCESS);
    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, replyText, replyLength, 0);
}

void systemMonitorHandler(ClientState *client)
{
    printf("[Handler 0x05] Processing System Monitor Request from FD #%d --- \n", client->fd);

    if (client->is_authenticated == 0)
    {
        printf("Denied! Unauthenticated monitor attempt on FD #%d\n", client->fd);
        struct messageHeader outboundHeader;
        char *replyText = "ERROR: AUTHENTICATION_REQUIRED";
        uint32_t replyLength = strlen(replyText);
        outboundHeader.messageLength = htonl(replyLength);
        outboundHeader.messageType = htonl(RES_ERR_DENIED);
        send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
        send(client->fd, replyText, replyLength, 0);
        return;
    }

    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    FILE *load_file = fopen("/proc/loadavg", "r");
    if (load_file != NULL)
    {
        fscanf(load_file, "%lf %lf %lf", &load1, &load5, &load15);
        fclose(load_file);
    }

    unsigned long memTotal = 0;
    unsigned long memFree = 0;
    char label[64];
    unsigned long val = 0;

    FILE *mem_file = fopen("/proc/meminfo", "r");
    if (mem_file != NULL)
    {
        while (fscanf(mem_file, "%s %lu kB", label, &val) != EOF)
        {
            if (strcmp(label, "MemTotal:") == 0)
            {
                memTotal = val / 1024;
            }
            if (strcmp(label, "MemFree:") == 0)
            {
                memFree = val / 1024;
            }
        }
        fclose(mem_file);
    }
    char replyText[1024];
    memset(replyText, 0, sizeof(replyText));
    snprintf(replyText, sizeof(replyText), "--- SYSTEM METRICS --- \nCPU LOAD AVG: %.2f, %.2f, %.2f\nMEMORY TOTAL: %lu MB\nMEMORY FREE: %lu MB\n", load1, load5, load15, memTotal, memFree);
    uint32_t replyLength = strlen(replyText);
    struct messageHeader outboundHeader;
    outboundHeader.messageLength = htonl(replyLength);
    outboundHeader.messageType = htonl(RES_SUCCESS);

    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, replyText, replyLength, 0);
}

void shellExecutionHandler(ClientState *client, char *command)
{
    printf("[Handler 0x06] Streaming Shell Command for FD #%d: '%s' --- \n", client->fd, command);

    if (client->is_authenticated == 0)
    {
        printf("Denied! Unauthenticated shell attempt on FD #%d\n", client->fd);
        struct messageHeader outboundHeader;
        char *replyText = "ERROR: AUTHENTICATION_REQUIRED";
        uint32_t replyLength = strlen(replyText);
        outboundHeader.messageLength = htonl(replyLength);
        outboundHeader.messageType = htonl(RES_ERR_DENIED);
        send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
        send(client->fd, replyText, replyLength, 0);
        return;
    }

    int pipeFds[2];
    if (pipe(pipeFds) < 0)
    {
        perror("Failed to create UNIX pipe");
        return;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("Failed to fork process");
        close(pipeFds[0]);
        close(pipeFds[1]);
        return;
    }
    if (pid == 0)
    {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);

        char *args[] = {"/bin/sh", "-c", command, NULL};
        execv("/bin/sh", args);
        exit(1);
    }

    close(pipeFds[1]);
    char streamBuffer[512];
    ssize_t bytesReadFromPipe;

    while ((bytesReadFromPipe = read(pipeFds[0], streamBuffer, sizeof(streamBuffer))) > 0)
    {
        struct messageHeader chunkHeader;
        chunkHeader.messageLength = htonl((uint32_t)bytesReadFromPipe);
        chunkHeader.messageType = htonl(RES_STREAM_DATA);

        send(client->fd, &chunkHeader, sizeof(chunkHeader), 0);
        send(client->fd, streamBuffer, bytesReadFromPipe, 0);
    }

    close(pipeFds[0]);
    int status;
    waitpid(pid, &status, 0);

    struct messageHeader endHeader;
    endHeader.messageLength = htonl((uint32_t)0);
    endHeader.messageType = htonl(RES_STREAM_END);
    send(client->fd, &endHeader, sizeof(endHeader), 0);
}

void fileUploadHandler(ClientState *client, char *buffer, uint32_t messageLength)
{
    char tempPaths[1024];
    memset(tempPaths, 0, sizeof(tempPaths));
    if (messageLength > 0 && messageLength < 1024)
    {
        memcpy(tempPaths, buffer, messageLength);
    }

    strtok(tempPaths, ":");
    char *remoteSourcePath = strtok(NULL, ":");

    printf("[SERVER UPLOAD] Pushing file out to client: %s\n", remoteSourcePath);

    FILE *file = fopen(remoteSourcePath, "rb");
    if (file == NULL)
    {
        printf("Error: Requested file '%s' missing on server disk.\n", remoteSourcePath);
        struct messageHeader errHeader;
        errHeader.messageType = htonl(FT_ERR);
        errHeader.messageLength = htonl(0);
        send(client->fd, &errHeader, sizeof(errHeader), 0);
        return;
    }

    fseek(file, 0, SEEK_END);
    uint64_t totalFileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    struct messageHeader outboundHeader;
    outboundHeader.messageType = htonl(FT_INIT);
    outboundHeader.messageLength = htonl(sizeof(struct fileInitPayload));

    struct fileInitPayload initPayload;
    initPayload.fileSize = htobe64(totalFileSize);

    const char *justFileName = strrchr(remoteSourcePath, '/');
    if (justFileName == NULL)
        justFileName = remoteSourcePath;
    else
        justFileName++;

    memset(initPayload.fileName, 0, sizeof(initPayload.fileName));
    strncpy(initPayload.fileName, justFileName, 255);

    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
    send(client->fd, &initPayload, sizeof(initPayload), 0);

    struct messageHeader initAckHeader;
    if (read_exact(client->fd, &initAckHeader, sizeof(initAckHeader)) <= 0 || ntohl(initAckHeader.messageType) != FT_INIT_ACK)
    {
        printf("Client failed initial download.\n");
        fclose(file);
        return;
    }

    char chunkDataBuffer[4096];
    uint32_t sequenceCounter = 0;
    size_t actualBytesRead;

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

            send(client->fd, &dataHeader, sizeof(dataHeader), 0);
            send(client->fd, &chunkHeader, sizeof(chunkHeader), 0);
            send(client->fd, chunkDataBuffer, actualBytesRead, 0);

            struct messageHeader chunkAckHeader;
            if (read_exact(client->fd, &chunkAckHeader, sizeof(chunkAckHeader)) > 0)
            {
                if (ntohl(chunkAckHeader.messageType) == FT_DATA_ACK)
                {
                    uint32_t clientConfirmedSeq;
                    read_exact(client->fd, &clientConfirmedSeq, sizeof(clientConfirmedSeq));
                    clientConfirmedSeq = ntohl(clientConfirmedSeq);

                    if (clientConfirmedSeq == sequenceCounter)
                    {
                        ackVerified = 1;
                    }
                }
            }

            if (!ackVerified)
            {
                transmissionRetries++;
                if (transmissionRetries > 5)
                {
                    printf("Timeout on chunk #%u. Aborting.\n", sequenceCounter);
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
    send(client->fd, &completionHeader, sizeof(completionHeader), 0);

    printf("Server upload processing done!\n");
    fclose(file);
}

void fileDownloadHandler(ClientState *client, char *buffer, uint32_t messageLength)
{
    char tempPaths[1024];
    memset(tempPaths, 0, sizeof(tempPaths));
    if (messageLength > 0 && messageLength < 1024)
    {
        memcpy(tempPaths, buffer, messageLength);
    }

    strtok(tempPaths, ":");
    strtok(NULL, ":");
    char *remoteTargetName = strtok(NULL, ":");
    if (remoteTargetName == NULL)
    {
        remoteTargetName = "uploaded.bin";
    }

    struct messageHeader outboundHeader;
    outboundHeader.messageType = htonl(FT_INIT_ACK);
    outboundHeader.messageLength = htonl(0);
    send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);

    FILE *destFile = NULL;
    uint32_t expectedSequence = 0;
    printf("Pulling file into: %s...\n", remoteTargetName);

    while (1)
    {
        struct messageHeader incomingHeader;
        int bytesRead = read_exact(client->fd, &incomingHeader, sizeof(incomingHeader));

        if (bytesRead <= 0)
        {
            printf("Error: Client Disconnected mid transfer.\n");
            if (destFile)
                fclose(destFile);
            return;
        }

        uint32_t frameType = ntohl(incomingHeader.messageType);

        if (frameType == FT_INIT)
        {
            struct fileInitPayload incomingPayloadHeader;
            read_exact(client->fd, &incomingPayloadHeader, sizeof(incomingPayloadHeader));
            uint64_t totalIncomingBytes = be64toh(incomingPayloadHeader.fileSize);

            printf("Allocating memory for file size: %lu bytes\n", totalIncomingBytes);
            destFile = fopen(remoteTargetName, "wb");

            struct messageHeader outboundAckHeader;
            outboundAckHeader.messageType = htonl(destFile ? FT_INIT_ACK : FT_ERR);
            outboundAckHeader.messageLength = htonl(0);
            send(client->fd, &outboundAckHeader, sizeof(outboundAckHeader), 0);

            if (!destFile)
                return;
        }
        else if (frameType == FT_DATA)
        {
            struct fileChunkHeader chunkHeader;
            read_exact(client->fd, &chunkHeader, sizeof(chunkHeader));
            uint32_t blockSequence = ntohl(chunkHeader.sequenceNumber);
            uint32_t rawDataLength = ntohl(chunkHeader.chunkDataLength);

            if (rawDataLength > MAX_CHUNK_CEILING)
            {
                fclose(destFile); return;
            }

            char *filePayloadBuffer = (char *)malloc(rawDataLength);
            if(!filePayloadBuffer){
                perror("Malloc Allocation Failed!");
                continue;
            }
            read_exact(client->fd, filePayloadBuffer, rawDataLength);

            if (blockSequence == expectedSequence)
            {
                fwrite(filePayloadBuffer, 1, rawDataLength, destFile);
                expectedSequence++;
            }
            free(filePayloadBuffer);

            struct messageHeader outboundSequenceAckHeader;
            outboundSequenceAckHeader.messageType = htonl(FT_DATA_ACK);
            outboundSequenceAckHeader.messageLength = htonl(sizeof(uint32_t));

            uint32_t outgoingAckSequence = htonl(expectedSequence - 1);
            send(client->fd, &outboundSequenceAckHeader, sizeof(outboundSequenceAckHeader), 0);
            send(client->fd, &outgoingAckSequence, sizeof(outgoingAckSequence), 0);
        }
        else if (frameType == FT_COMP)
        {
            if (destFile)
                fclose(destFile);
            printf("Server download finished successfully!\n");
            break;
        }
    }
}

int main(void)
{
    int numberOfPorts = MAX_PORTS;
    for (int i = 0; i < numberOfPorts; i++)
    {
        serverFds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFds[i] < 0)
        {
            perror("Socket creation failed!");
            return -1;
        }
        if (setsockopt(serverFds[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        {
            perror("setsockopt SO_REUSEADDR failed!");
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(securePorts[i]);

        if (bind(serverFds[i], (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            perror("Bind failed!");
            close(serverFds[i]);
            continue;
        }

        if (listen(serverFds[i], 10) < 0)
        {
            perror("Listen failed!");
            close(serverFds[i]);
            continue;
        }
    }

    int epollFd = epoll_create1(0);
    if (epollFd < 0)
    {
        perror("Failed to create an epoll instance!");
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;

    for (int i = 0; i < MAX_PORTS; i++)
    {
        if(serverFds[i] < 0){
            continue;
        }
        ev.data.fd = serverFds[i];
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFds[i], &ev) < 0)
        {
            perror("Failed to add an epoll instance in epollFd!");
            return -1;
        }
    }

    struct epoll_event userEpollScreen[MAX_EVENTS];
    printf("Protocol Engine Active on Ports 8080, 9000, 9999. Awaiting Commands...\n");

    while (1)
    {
        int readySlots = epoll_wait(epollFd, userEpollScreen, MAX_EVENTS, -1);
        if (readySlots < 0)
        {
            perror("Failed to gather ready clients!");
            break;
        }

        for (int i = 0; i < readySlots; i++)
        {
            int activeClientFd = userEpollScreen[i].data.fd;

            int isServerFd = 0;
            uint16_t matchedPort = 0;
            for (int j = 0; j < MAX_PORTS; j++)
            {
                if (activeClientFd == serverFds[j])
                {
                    isServerFd = 1;
                    matchedPort = securePorts[j];
                    break;
                }
            }

            if (isServerFd == 1)
            {
                struct sockaddr_in client_addr;
                memset(&client_addr, 0, sizeof(client_addr));
                socklen_t clientLen = sizeof(client_addr);

                int newClientFd = accept(activeClientFd, (struct sockaddr *)&client_addr, &clientLen);
                if (newClientFd < 0)
                {
                    perror("Accept Failed!");
                    continue;
                }
                printf("Accepted client #%d\n", newClientFd);

                ClientState *state = malloc(sizeof(ClientState));
                if (state == NULL)
                {
                    perror("Failed to allocate memory for ClientState!");
                    close(newClientFd);
                    continue;
                }

                state->fd = newClientFd;
                state->is_authenticated = 0;
                state->connected_port = matchedPort;

                struct epoll_event newClientEv;
                newClientEv.events = EPOLLIN;
                newClientEv.data.ptr = state;

                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, newClientFd, &newClientEv) < 0)
                {
                    perror("Failed to add an epoll instance in epollFd!");
                    close(newClientFd);
                    free(state);
                    continue;
                }
            }
            else
            {
                ClientState *client = (ClientState *)userEpollScreen[i].data.ptr;
                struct messageHeader incomingHeader;

                int headerBytes = read_exact(client->fd, &incomingHeader, sizeof(incomingHeader));
                if (headerBytes <= 0)
                {
                    printf("Client Disconnected during header read!\n");
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, client->fd, NULL);
                    close(client->fd);
                    free(client);
                }
                else
                {
                    uint32_t messageLength = ntohl(incomingHeader.messageLength);
                    uint32_t messageType = ntohl(incomingHeader.messageType);
                    char buffer[1024] = {0};

                    if (messageLength >= 1024)
                    {
                        printf("Client #%d sent a packet way too big! Disconnecting Client!", client->fd);
                        epoll_ctl(epollFd, EPOLL_CTL_DEL, client->fd, NULL);
                        close(client->fd);
                        free(client);
                        continue;
                    }

                    int bodyCheck = 1;
                    if (messageLength > 0)
                    {
                        int bodyBytes = read_exact(client->fd, buffer, messageLength);
                        if (bodyBytes <= 0)
                        {
                            printf("Client Disconnected during body payload read!\n");
                            epoll_ctl(epollFd, EPOLL_CTL_DEL, client->fd, NULL);
                            close(client->fd);
                            free(client);
                            bodyCheck = 0;
                        }
                    }

                    if (bodyCheck == 1)
                    {
                        switch (messageType)
                        {
                        case OP_PING:
                            pingHandler(client);
                            break;
                        case OP_AUTH_REQ:
                            authorisationHandler(client, buffer, messageLength);
                            break;
                        case OP_CMD_REQ:
                            commandHandler(client, buffer);
                            break;
                        case OP_FILE_INIT:
                            fileTransferInitialisationHandler(client);
                            break;
                        case OP_SYS_MONITOR:
                            systemMonitorHandler(client);
                            break;
                        case OP_SHELL_EXEC:
                            shellExecutionHandler(client, buffer);
                            break;
                        case OP_FILE_TRANSFER:
                            if (client->is_authenticated == 0)
                            {
                                printf("Denied! Unauthenticated file transfer attempt on FD #%d\n", client->fd);
                                struct messageHeader outboundHeader;
                                char *replyText = "ERROR: AUTHENTICATION_REQUIRED";
                                uint32_t replyLength = strlen(replyText);
                                outboundHeader.messageLength = htonl(replyLength);
                                outboundHeader.messageType = htonl(RES_ERR_DENIED);
                                send(client->fd, &outboundHeader, sizeof(outboundHeader), 0);
                                send(client->fd, replyText, replyLength, 0);
                                break;
                            }
                            char directionCheck[1024] = {0};
                            if (messageLength > 0 && messageLength < 1024)
                            {
                                memcpy(directionCheck, buffer, messageLength);
                            }
                            char *tag = strtok(directionCheck, ":");

                            if (tag != NULL && strcasecmp(tag, "DOWN") == 0)
                            {
                                fileUploadHandler(client, buffer, messageLength);
                            }
                            else if (tag != NULL && strcasecmp(tag, "UP") == 0)
                            {
                                fileDownloadHandler(client, buffer, messageLength);
                            }
                            break;
                        default:
                            printf("Unknown opcode (0x%X) on FD #%d!\n", messageType, client->fd);
                            struct messageHeader errHeader;
                            errHeader.messageLength = htonl(0);
                            errHeader.messageType = htonl(RES_ERR_FAIL);
                            send(client->fd, &errHeader, sizeof(errHeader), 0);
                            break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}