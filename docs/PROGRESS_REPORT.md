# Project progress report

The server and client implement the file-sharing assignment. The server
manages accounts, active peer endpoints, advertisements, and service logs.
Clients save their local catalogs, find other peers by exact filename, and
transfer files directly.

## Completed requirements

### Cao Trường Xuân (20236016)

Responsible for the project foundation and Makefile, the control server,
shared networking, and server-side tests.

| Requirement | Points | Implementation |
| --- | ---: | --- |
| Stream-based data handling | 1 | Buffered TCP reads reconstruct CRLF lines and preserve bytes for subsequent reads. |
| Server-side socket I/O | 2 | The server handles concurrent control connections and pipelined requests. |
| Account registration and management | 2 | Registration validates credentials and persists salted PBKDF2-HMAC-SHA256 hashes. |
| Login and session management | 2 | Login activates a peer announcement. Logout and disconnect release the active ID and advertisements. |
| Client activity storage | 1 | The server tracks active endpoints and shares in memory and stores connection and request events on disk. |
| Search by filename | 1 | Exact-name search excludes the requester and returns a count followed by matching peer endpoints. |
| Service logging | 1 | The server writes UTC timestamps for startup, shutdown, connections, and request outcomes. |
| **Assigned total** | **10** | |

### Nguyễn Đức Thắng (20235998)

Responsible for the command-line client, local state, peer transfers, and
client-side tests.

| Requirement | Points | Implementation |
| --- | ---: | --- |
| Client-side socket I/O | 2 | The client maintains a control connection, opens direct peer connections, and accepts uploads through four workers. |
| Announce client ID and listening port | 1 | The client saves a random unsigned 32-bit ID in `config.txt` and sends an automatic `HELLO` when it connects. |
| Register filenames for sharing | 2 | The client saves basenames and absolute paths in `index.txt` and advertises readable files under its active ID. |
| Unregister filenames from sharing | 2 | The client saves the catalog removal before withdrawing its advertisement and retries rejected withdrawals while connected. |
| Direct file downloading | 3 | The client streams binary data from the selected peer, publishes complete files without overwriting, and reports peer failures to the server. |
| **Assigned total** | **10** | |

Both members share responsibility for the wire protocol, integration,
verification, and troubleshooting. The completed implementation covers all
12 requirements in the 20-point rubric.

## Verification

Automated tests ran on Ubuntu 24.04.1 LTS under WSL. The account-store
permissions case ran separately with privilege dropping enabled.

| Check | Result |
| --- | --- |
| `make test` | All three C suites and 93 Python tests passed, including the separate permissions case. |
| AddressSanitizer and UndefinedBehaviorSanitizer | The same suites passed with sanitizers enabled. |
| Isolated LAN | Two peers on separate IPv4 addresses transferred a 2,097,408-byte file byte-for-byte. Search excluded the requester, and the owner withdrew the advertisement after its source was removed. |

The LAN test used isolated network namespaces on one host. Acceptance on two
physical workstations is pending.

See the [build and usage guide](../README.md), [protocol](PROTOCOL.md), and
[error handling guide](ERROR_HANDLING.md) for implementation details.
