# p2p-file-sharing

A C11 file-sharing project for Ubuntu 20.04 with POSIX TCP sockets and pthreads.
The central server handles accounts, sessions, peer announcements, file
discovery, and service logging. Clients transfer files directly between peers.

## Build and run

Install `build-essential libssl-dev python3`, then run the build and tests.

```sh
make
make test
./server
```

The server defaults to `127.0.0.1:9000` and creates
`data/server/accounts.db` as needed. Use `--bind`, `--port`, `--accounts`,
or `--log` to override defaults. Custom storage needs an existing trusted
parent directory. The server prints `READY` once it starts accepting
connections. SIGINT and SIGTERM interrupt active sockets and join workers.

Start each client in a separate terminal. The example uses separate peer ports
and local state. Create the download directories before starting the clients.

```sh
./client --peer-port 9001 --config peer-a.txt --index shares-a.txt --downloads downloads-a
./client --peer-port 9002 --config peer-b.txt --index shares-b.txt --downloads downloads-b
```

The client defaults to the local server, peer port `9001`, ID file
`config.txt`, catalog `index.txt`, and download directory `.`. Use
`--server IPv4` and `--port PORT` to choose the control endpoint.

| Command | Action |
| --- | --- |
| `register USER` | Create an account |
| `login USER` | Log in and announce the peer listener |
| `hello` | Retry the peer announcement and reannounce shares |
| `logout` | End the session and release the client ID |
| `share PATH` | Save a local file to the catalog and advertise it when online |
| `unshare NAME` | Remove an advertisement and its local catalog entry |
| `shares` | List local files as pending or announced |
| `search NAME` | Find peers that share the exact filename |
| `download NUMBER` | Download a numbered result from the latest search |
| `help` | Show commands |
| `quit` | Close the connection and exit |

The terminal hides passwords. With redirected input, put the password on the
line after the account command. File arguments use the literal remainder of
the line, so spaces work without quotes. Shared filenames are printable ASCII
basenames. Each numbered search result shows the filename, client ID, IP
address, and port. Use the result number with `download NUMBER`. The client
prints `200 END` when the search is complete.

Local shares survive logout and restart. A failed download is displayed locally
and its temporary file is removed. A completed download cannot overwrite an
existing path. Ctrl+C or SIGTERM stops a pending download, restores terminal
echo, and joins the client workers.

## Protocol and storage

Both control and peer headers use CRLF and three-digit replies. The tracker
sends one `FILE` line per search match with the size recorded at announcement,
followed by `200 END`. Peers read up to the source's EOF and send the bytes in
length-prefixed `<data>` frames and a final `<end>` frame. The downloader
publishes the bytes it received once the final frame is complete. Both peers
need the updated client for this transfer format.
See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full contract.

Account storage uses salted PBKDF2-HMAC-SHA256, private permissions, an
exclusive lock, and a durable append before registration succeeds. Existing
account records and local catalogs retain their format. The server logs
connection activity and successful account, announcement, and file operations.
The peer registry and file index live in memory. The server copies search
matches under a mutex and sends them after releasing the lock.

Use this IPv4 service on a controlled network. Credentials travel in plaintext.
Socket timeouts bound inactivity, and available memory and OS resources govern
how many independent sessions, entries, and bytes can be handled.

## Coursework references

HW1 supplied the dynamic input, account/session separation, and logging
patterns. HW2 and HW3 reinforced validating addresses, ports, and input before
acting, and checking socket results. HW4 demonstrated buffered line and binary
reads through one TCP reader. HW5–HW7 reinforced CRLF framing, numeric
responses, connection-scoped state, and cleanup, with pthreads in HW6 and
poll-based socket and terminal handling in HW7.

## Verification

`make test` runs C socket, protocol, and file tests alongside Python TCP
integration tests. The cases include fragmented CRLF, pipelined requests, long
input, empty and interrupted searches, concurrent sessions and index updates,
restart and storage failure, exact binary transfers, and signal cleanup.

An AddressSanitizer and UndefinedBehaviorSanitizer build uses the same suite.

```sh
make clean
make test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-pthread -fsanitize=address,undefined -no-pie'
```

## Team Members & Task Assignments

The original task split is listed below.

| Member | Full Name | Student ID | Role | Total Points |
| --- | --- | --- | --- | --- |
| Member 1 | Cao Trường Xuân | 20236016 | Server Development & Shared Networking | 10 |
| Member 2 | Nguyễn Đức Thắng | 20235998 | Client Development & P2P File Transfer | 10 |
| **Total** | | | | **20** |

### Member 1 — Cao Trường Xuân

**Role:** Server Development & Shared Networking

**Graded tasks**

| No. | Assigned Task | Points |
| --- | --- | --- |
| 1 | Line-oriented data processing | 1 |
| 2 | Server socket I/O implementation | 2 |
| 3 | User registration and account management | 2 |
| 4 | User login and session management | 2 |
| 5 | Client activity information storage | 1 |
| 6 | Server-side file searching | 1 |
| 7 | Service logging | 1 |
| | **Total** | **10** |

**Additional responsibilities**

- Initialize the project skeleton and Makefile.
- Define the shared TCP communication protocol.
- Implement the multithreaded server.
- Maintain the centralized file index.
- Handle shared-file registration and removal requests on the server.
- Implement server-side tests and maintain server modules.

### Member 2 — Nguyễn Đức Thắng

**Role:** Client Development & P2P File Transfer

**Graded tasks**

| No. | Assigned Task | Points |
| --- | --- | --- |
| 1 | Client socket I/O implementation | 2 |
| 2 | Sending client ID and P2P listening port | 1 |
| 3 | Registering shared filenames | 2 |
| 4 | Unregistering shared filenames | 2 |
| 5 | Peer-to-peer file downloading | 3 |
| | **Total** | **10** |

**Additional responsibilities**

- Implement the client interface and server communication.
- Generate and persist client IDs in `config.txt`.
- Maintain the local shared-file index in `index.txt`.
- Implement client-side account registration, login, and file-search requests.
- Implement direct P2P upload and download.
- Implement client-side tests and maintain client modules.

### Shared Responsibilities

Both members are responsible for:

- Following the agreed TCP communication protocol.
- Maintaining C11 compatibility with Ubuntu 20.04.
- Testing their assigned components.
- Reviewing GitHub pull requests.
- Integrating and testing the complete application.
- Debugging cross-component issues.
