# Custom Multi-Protocol Remote Management Server

A custom remote management server built from scratch in C, running on Linux using POSIX sockets and epoll. No HTTP, no third-party networking libraries, no abstractions — everything is built directly on raw TCP.

---

## What This Is

This is a fully functional remote management system built on a custom binary protocol over TCP. The server runs on Linux and handles multiple simultaneous clients using epoll-based I/O multiplexing. The client runs on Linux using the same POSIX socket API.

This is not a tutorial project. Every architectural decision — the framing protocol, the epoll dispatch engine, the authentication handshake, the streaming model, the file transfer state machine — was designed and stress-tested from first principles.

---

## Architecture

### Protocol Design

Every message on the wire is a **length-prefixed binary frame**:

```
[ messageLength : 4 bytes ] [ messageType : 4 bytes ] [ body : N bytes ]
```

All multi-byte fields are transmitted in network byte order (big-endian). The server reads the fixed-size header first, extracts the body length, then reads exactly that many bytes. This eliminates ambiguity in TCP stream parsing and handles partial reads correctly via a `read_exact` loop.

### Server Design

The server is built around Linux's `epoll` for non-blocking I/O multiplexing. A single event loop handles all clients simultaneously — new connections, incoming frames, and disconnections — without blocking on any single client.

Each accepted client is allocated a `ClientState` structure on the heap containing:
- File descriptor
- Authentication status flag
- Connected port

This structure is stored directly in the epoll event's `data.ptr` field, eliminating the need for a separate client lookup table.

### Command Dispatch

Incoming frames are routed to handler functions based on their opcode. Every handler receives a pointer to the client's `ClientState`. Authentication is enforced at the handler level — unauthenticated clients receive `RES_ERR_DENIED` regardless of the opcode sent.

---

## Opcodes

### Client → Server

| Opcode | Hex | Description |
|--------|-----|-------------|
| OP_PING | 0x01 | Liveness check |
| OP_AUTH_REQ | 0x02 | Authentication request |
| OP_CMD_REQ | 0x03 | Static command execution |
| OP_FILE_INIT | 0x04 | Legacy stream initialisation |
| OP_SYS_MONITOR | 0x05 | Request system metrics |
| OP_SHELL_EXEC | 0x06 | Remote shell command execution |
| OP_FILE_TRANSFER | 0x07 | File upload or download |

### Server → Client

| Opcode | Hex | Description |
|--------|-----|-------------|
| RES_SUCCESS | 0x101 | Generic success |
| RES_AUTH_OK | 0x102 | Authentication accepted |
| RES_ERR_FAIL | 0x103 | Execution error |
| RES_ERR_DENIED | 0x104 | Access denied |
| RES_STREAM_DATA | 0x201 | Shell output chunk |
| RES_STREAM_END | 0x202 | Stream termination signal |
| FT_INIT | 0x301 | File transfer initiation |
| FT_INIT_ACK | 0x302 | File transfer acknowledgement |
| FT_DATA | 0x303 | File data chunk |
| FT_DATA_ACK | 0x304 | Chunk acknowledgement |
| FT_COMP | 0x305 | Transfer complete |
| FT_ERR | 0x306 | Transfer error |

---

## Modules

### Authentication (Stage 5)
Challenge-response authentication using a shared secret. Unauthenticated clients cannot access any module. Authentication state is tracked per-client in `ClientState.is_authenticated`. All handlers enforce this flag independently.

### System Monitoring (Stage 6)
Reads live system metrics from the Linux `/proc` filesystem — CPU load averages from `/proc/loadavg` and memory statistics from `/proc/meminfo`. Returns serialised metrics as a single response frame.

### Remote Shell Execution (Stage 7)
Executes arbitrary shell commands on the server via `fork()` + `execv()` + `pipe()`. stdout and stderr are captured and streamed back to the client in real time as `RES_STREAM_DATA` frames, terminated by `RES_STREAM_END`. Child processes are reaped with `waitpid()` to prevent zombie accumulation.

### File Transfer (Stage 8)
Bidirectional file transfer over a custom sub-protocol within the `OP_FILE_TRANSFER` opcode. Supports both upload (client → server) and download (server → client).

The transfer protocol:
```
Sender                          Receiver
  |--- FT_INIT (metadata) ------->|
  |<-- FT_INIT_ACK ---------------|
  |--- FT_DATA (chunk #0) ------->|
  |<-- FT_DATA_ACK (seq #0) ------|
  |--- FT_DATA (chunk #1) ------->|
  |<-- FT_DATA_ACK (seq #1) ------|
  |         ...                   |
  |--- FT_COMP ------------------->|
```

Each chunk carries a sequence number. The receiver verifies sequence order and sends per-chunk acknowledgements. The sender retransmits unacknowledged chunks up to 5 times before aborting. Chunk size is bounded by `MAX_CHUNK_CEILING` (65536 bytes) to prevent memory exhaustion from malformed frames.

---

## Client Commands

```
PING                          Liveness check
AUTH <password>               Authenticate with the server
COMMAND GET_STATUS            Request server status
MONITOR                       Request live system metrics
EXEC <shell command>          Execute a shell command and stream output
UPLOAD <local_path> <name>    Upload a file to the server
DOWNLOAD <remote_path> <name> Download a file from the server
EXIT                          Close the connection
```

---

## Building

```bash
gcc Mi_Servidor.c -o Mi_Servidor
gcc Mi_Cliente.c -o Mi_Cliente
```

No external dependencies. Standard C99, Linux kernel 2.6.17+ (epoll_create1).

---

## Running

Terminal 1 — start the server:
```bash
./Mi_Servidor
```

Terminal 2 — connect the client:
```bash
./Mi_Cliente
```

The server listens on ports 8080, 9000, and 9999 simultaneously.

---

## Design Decisions Worth Noting

**Why epoll over select/poll**
`select` and `poll` scale linearly with the number of monitored file descriptors. `epoll` scales with the number of *active* events. For a server expecting many idle connections and bursts of activity, epoll is the correct choice.

**Why a custom binary protocol over text**
Text protocols require delimiter parsing, are ambiguous at boundaries, and carry significant overhead for binary payloads like file transfer. A length-prefixed binary protocol is unambiguous, efficient, and trivially extensible via new opcodes.

**Why length-prefixed framing**
TCP is a stream protocol. It provides no message boundaries. Without explicit framing, a receiver cannot determine where one message ends and the next begins. The `read_exact` function guarantees that every logical message is read completely regardless of how TCP segments the data.

**Why per-handler authentication enforcement**
Centralising auth at the dispatch layer is simpler but creates a single point of failure. Enforcing the `is_authenticated` flag in each handler independently means a logic error in the dispatch layer cannot silently grant access to protected operations.

---

## Known Limitations

- Shell execution blocks the entire server for the duration of the command. During an `EXEC` call no other clients are serviced. This is a consequence of synchronous pipe reading inside the epoll loop.
- No command allowlist or denylist. Any authenticated client can execute any shell command.
- Plaintext authentication. The shared secret is transmitted without encryption.
- No connection timeout. Silent clients hold their file descriptor indefinitely.

All of the above are intentional deferments to Stage 10 hardening.

---

## Future Implementation

### Stage 9 — Real Internet Connectivity
- Port forwarding configuration on the host router
- Public IP binding and NAT traversal
- Testing all modules over live internet connections
- Handling new failure modes introduced by real network conditions — latency, packet reordering, mid-transfer disconnections
- ISP-level firewall and CGNAT identification and workarounds

### Stage 10 — Hardening and Stress Testing
- Command allowlist enforcement to prevent destructive or blocking shell commands
- Non-blocking pipe reads in `shellExecutionHandler` registered back into epoll, eliminating the server-blocking issue
- Connection timeout — clients silent for N seconds are disconnected and their state cleaned up
- `SIGTERM` and `SIGINT` signal handlers for graceful server shutdown with proper resource cleanup
- Fuzzing the protocol — malformed headers, invalid opcodes, zero-length bodies, oversized length fields
- Race condition and memory leak auditing under Valgrind
- `/proc/meminfo` parsing hardened to handle non-standard line formats
- Wrapper structures for server file descriptors to eliminate the `data.fd`/`data.ptr` union aliasing in the epoll event loop
- TLS encryption layer over the existing TCP transport

---

## What This Project Covers

- POSIX socket API — `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`
- Linux epoll — `epoll_create1`, `epoll_ctl`, `epoll_wait`, edge and level triggering
- TCP stream framing and partial read handling
- Binary protocol design — opcodes, length-prefixed frames, network byte order
- Per-client state management with heap-allocated structures
- UNIX process management — `fork`, `execv`, `pipe`, `dup2`, `waitpid`
- File I/O — `fopen`, `fread`, `fwrite`, `fseek`, `ftell`
- Endianness — `htonl`, `ntohl`, `htobe64`, `be64toh`
- Memory management — `malloc`, `free`, null checks, bounds validation
