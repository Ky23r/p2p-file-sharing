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
The client generates a random 32-bit ID on first run and sends it with its
listening port when it connects. Login activates that announcement and
publishes saved shares.

For a LAN check with two workstations, replace `192.168.1.20` with the server's
LAN IPv4 address. Run the first command on the server and one client command on
each workstation. Use separate accounts and allow TCP connections on the
server port and the peer listener ports.

```sh
./server --bind 192.168.1.20 --port 9000
./client --server 192.168.1.20 --port 9000 --peer-port 9001
./client --server 192.168.1.20 --port 9000 --peer-port 9002
```

| Terminal command | Action |
| --- | --- |
| `register USER` | Create an account |
| `login USER` | Log in and publish saved shares |
| `hello` | Refresh the peer announcement and retry saved shares |
| `logout` | Log out and release the client ID |
| `share PATH` | Save a local file and advertise it when logged in |
| `unshare NAME` | Remove an advertisement and its local catalog entry |
| `shares` | List local files as pending or announced |
| `search NAME` | Find peers that share the exact filename |
| `download NUMBER` | Download a result from the latest search |
| `help` | Show commands |
| `quit` | Close the connection and exit |

The terminal hides passwords. With redirected input, put the password on the
line after the account command. File arguments use the literal remainder of
the line, so spaces work without quotes. Filenames may contain non-ASCII
characters. Each numbered search result shows the filename, client ID, IP
address, and port. Use the result number with `download NUMBER`. The client
prints `200 OK` when the search is complete. Terminal commands use lowercase.
`help` and `quit` are local to the client. Both executables support `--help`.

Local shares survive logout and restart. The client prints download failures
and removes partial files. When an upload request reveals a missing or
inaccessible source, the owner withdraws that listing with
`UNSHARE` and keeps its local catalog entry pending. A completed download
cannot overwrite an existing path. Ctrl+C or SIGTERM stops a pending
download, restores terminal echo, and joins the client workers.

`unshare` saves the local removal before requesting removal on the server. If
the save fails, the file stays shared. After a successful save, the client
stops accepting new downloads for that file and retries a rejected server
withdrawal while the control session stays active. A later successful `share`
of the same name replaces the old advertisement and cancels its queued removal.

## Protocol and storage

Both channels use CRLF and case-insensitive command verbs. Replies begin with
a three-digit numeric code. Search sends one `200-` continuation with the
filename, size, client ID, address, and port per result, then ends with
`200 OK`. Peer downloads start with `200 CHUNKED`. The uploader reads the file
through EOF and sends length-prefixed `<data>` frames and a final `<end>`
frame. The downloader publishes the file after receiving the final frame.
Upgrade the server and all clients together when switching wire formats.
The full contract is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Account storage uses salted PBKDF2-HMAC-SHA256, private permissions, an
exclusive lock, and a durable append before registration succeeds. Existing
account records and local catalogs retain their format. The server logs
connection activity and request outcomes. The peer registry and file index
live in memory. The server copies search matches under a mutex and sends
them after releasing the lock.

Use this IPv4 service on a controlled network. Credentials travel in plaintext.
Socket timeouts bound inactivity, and available memory and OS resources govern
how many independent sessions, entries, and bytes can be handled.

## Verification

`make test` runs C socket, protocol, and file tests alongside Python TCP
integration tests. The cases include fragmented CRLF, pipelined requests, long
input, mixed-case verbs, malformed replies, empty and interrupted multiline
searches, concurrent sessions and index updates, restart and storage failure,
exact binary transfers, stale advertisement withdrawal, and signal cleanup.

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
