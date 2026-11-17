# p2p-file-sharing

C11 project targeting Ubuntu 20.04, GCC, POSIX TCP sockets, and pthreads.
The centralized control server provides registration, authentication, and
connection-local sessions. The terminal client sends account commands and
announces its persistent peer identity to the server.

The client also implements a persistent local share catalog, file-discovery
requests, and direct peer uploads/downloads. **The production server's file-index,
search, and failure-report handlers are still pending Member 1 work.** The new
client behavior is tested against a test-only discovery server; the existing
production server continues to support account/session commands. The complete
integration contract is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Install `build-essential libssl-dev python3`, then run:

```sh
make
make test
./server --help
./server
# Or choose an IPv4 address, port, and account file:
./server --bind 127.0.0.1 --port 9100 --accounts /private/directory/accounts.db
make clean
```

The build uses `-std=c11 -Wall -Wextra -Wpedantic -pthread` and links `libcrypto`.
Generated files and runtime account storage are ignored by Git.

The default endpoint is `127.0.0.1:9000`; default storage is
`data/server/accounts.db` (created on first start). Custom storage requires an
existing, trusted parent directory. Run as an ordinary user. SIGINT or SIGTERM
stops accepting clients, shuts down active sockets, joins every worker, and
releases storage. The server allows 128 simultaneous connections and applies
30-second receive/send inactivity timeouts. A timed-out socket closes without a
protocol response; sending bytes slowly can extend a request's total lifetime.
Startup prints the bound endpoint and `READY` once signal handling is installed.

## Client core

After starting the server, run `./client` in a second terminal. The default
settings can also be supplied explicitly.

```sh
./client --server 127.0.0.1 --port 9000 --peer-port 9001 --config config.txt
```

| Command | Action |
| --- | --- |
| `register alice` | Prompt for a password and create an account |
| `login alice` | Prompt for a password, log in, and send `HELLO` |
| `hello` | Send the client ID and peer port again on the current connection |
| `logout` | End the session and release the client ID |
| `share PATH` | Persist a local file and announce it when the peer is registered |
| `unshare NAME` | Remove a share from the server and local catalog |
| `shares` | List local files as `PENDING` or `ANNOUNCED` |
| `search QUERY` | Search filenames and display numbered peers |
| `download NUMBER` | Download a result from the latest search |
| `help` | Show commands |
| `quit` | Close the connection and exit |

The terminal hides password input. With redirected stdin, put the password on
the line after `register USER` or `login USER`. The client displays the server's
`OK` and `ERR` responses. A connection error ends the CLI, so start a new client
session to reconnect.

`config.txt` holds an unsigned 32-bit client ID as one line of up to ten decimal
digits. The client generates the ID when the file is missing and reuses it on
later runs. Use a separate `--config` path in a trusted directory for each peer.
If the server returns `CLIENT_ID_IN_USE`, the login still succeeds and `hello`
can retry the announcement.

The client opens an IPv4 TCP listener on `--peer-port` (default `9001`) before
connecting to the control server. `HELLO` announces that port after login.
Four upload workers accept peer requests and serve files that have been announced
successfully on the active control session. An idle authenticated client repeats
HELLO every 10 seconds so the control connection survives the server's 30-second
inactivity limit. Transport failures disable new uploads and end the CLI.

`--index PATH` selects the local catalog (default `index.txt`), and `--downloads
DIRECTORY` selects an existing trusted download directory (default `.`). Use
distinct peer ports, config files, catalogs, and download directories for multiple
clients. File arguments are the literal remainder of a command line, so spaces
and UTF-8 names work without quotes. Share names are canonical path basenames.

```sh
./client --peer-port 9001 --config peer-a.txt --index shares-a.txt --downloads downloads-a
./client --peer-port 9002 --config peer-b.txt --index shares-b.txt --downloads downloads-b
```

The directories must already exist. With the server extension integrated, use
`share /path/to/report.pdf`, `search report`, then `download 1` on the receiving
client. On the current production server, file control requests return errors
because those handlers are not implemented yet. `tests/client/control_fixture.py`
implements the contract only for automated tests; it accepts test logins and must
not be used as a real account server.

Local shares survive logout/restart and are reannounced after login/HELLO. Missing
or rejected files remain pending and cannot be served. `unshare` preserves the
source file. An already opened upload may finish after unshare/logout. Downloads
use bounded binary buffers, require exact size and clean EOF, and publish a
mode-0600 completed file without overwriting an existing path. Failed transfers
remove their temporary file and report the peer failure when control is available;
local storage errors are not reported as peer failures. No resume or content hash
is provided; shared sources must remain unchanged during transfer.

Ctrl+C and SIGTERM cancel a pending download, remove its partial temporary file,
restore terminal echo, and stop/join client workers before exiting with 130/143.
Cancellation is local and is not reported as a failed peer. A successfully
published complete file is retained.

## Control protocol

Each ASCII request ends with LF, with at most 4096 bytes **excluding LF**.
Commands are case-sensitive. Arguments use exactly one ASCII space. Leading or
trailing spaces, CR (including CRLF), tabs, NULs, and other control bytes are
invalid. Responses end with LF: `OK <MESSAGE>` or `ERR <ERROR_CODE>`.

| Request | Success | Authentication |
| --- | --- | --- |
| `REGISTER <username> <password>` | `OK REGISTERED` | Logged out; remains logged out |
| `LOGIN <username> <password>` | `OK LOGGED_IN` | Logged out; logs in this connection |
| `HELLO <client_id> <port>` | `OK HELLO` | Login required |
| `LOGOUT` | `OK LOGGED_OUT` | Login required; clears session, keeps TCP open |

Usernames are case-sensitive, 3–32 ASCII letters, digits, or underscores.
Passwords are 8–128 ASCII bytes from `!` through `~` (no spaces).
Client IDs are unsigned decimal integers from 0 through 4294967295; ports are
decimal integers from 1 through 65535. Signs are invalid; leading zeros are
allowed. HELLO records the IP observed on the connection. A client may update
its own registration; another active session with the same ID is rejected.

| Error | Meaning |
| --- | --- |
| `INVALID_REQUEST` | Invalid bytes, spacing, argument count, or argument values |
| `UNKNOWN_COMMAND` | Unrecognized command in an otherwise valid request |
| `AUTH_REQUIRED` | HELLO or LOGOUT before login |
| `ALREADY_AUTHENTICATED` | LOGIN or REGISTER while logged in |
| `USER_EXISTS` | Username already registered |
| `INVALID_CREDENTIALS` | Unknown username or wrong password (same response) |
| `CLIENT_ID_IN_USE` | Another active connection holds the ID |
| `LINE_TOO_LONG` | Request exceeds 4096 bytes; connection closes |
| `SERVER_BUSY` | Connection/thread capacity unavailable; connection closes |
| `INTERNAL_ERROR` | Credential hashing or persistent-storage failure |

Syntax validation precedes authorization checks. Requests have at most two
arguments, even for unknown commands. Malformed lines can be followed by more
commands. Oversized lines return `LINE_TOO_LONG` and close the connection; EOF discards incomplete
requests. Disconnects clear all session state. Accounts survive restarts.
LOGIN does not automatically register a peer. One account may log in on multiple
connections; authentication and HELLO state remain independent. A rejected
HELLO leaves the connection's previous registration intact. LOGOUT and disconnect
release the ID; IDs and peer details are never persisted.

Plaintext TCP authentication is acceptable only for controlled local testing.
External deployment requires encrypted transport. This core has no login rate
limiter or account recovery mechanism and is not an Internet deployment package.

## Accounts and modules

`src/server/auth.c` uses OpenSSL PBKDF2-HMAC-SHA256 with 600,000 iterations,
16-byte random salts, 32-byte hashes, and constant-time hash comparison. Missing
users still incur the KDF. Passwords are never persisted or logged. The storage
format is `username:600000:salt_hex:hash_hex` followed by LF. The file is restricted
to mode 0600 and locked exclusively for the process lifetime. Symlinks, hard
links, non-regular files, and files owned by another UID are rejected.

Registrations are serialized at persistence, appended and synced before success;
failed/partial appends are rolled back. Failed rollback disables further account
operations. Invalid or truncated records cause a clean startup failure without
rewriting their contents. Back up the account file; repair corruption offline.
There is no automatic recovery from a crash during an append.

`src/server/server.c` owns IPv4 sockets, strict parsing, workers, and the shutdown
self-pipe. `src/server/session.c` owns the mutex-protected session registry and
client ID uniqueness. Completed workers are regularly joined and freed; shutdown
also waits for credential operations already in progress. Auth and session locks
are never held together; expensive KDF work runs outside both locks.

`make test` runs C socket/catalog/upload tests and Python standard-library TCP integration
tests using temporary data and available local ports. Client tests cover account
commands, reachable peer announcements, port conflicts, ID persistence, terminal
input, and invalid server replies. Server tests cover concurrent accounts and sessions, persistence and hash
verification, malformed input, storage failures, client limits, descriptor/thread
cleanup, and graceful shutdown.
`tests/client/test_client.py` runs real client processes against a discovery
fixture and real peer sockets: registration/removal/search, source selection,
binary/empty/fragmented transfers, parallel uploads, malformed headers/results,
partial-transfer cleanup/reporting, no-clobber destinations, persistent catalogs,
reannouncement, idle keepalive, and prompt shutdown on control failure. The fixture
does not replace testing the production server extension once Member 1 adds it.
The permission-denial test drops privileges when run as root; it is skipped for
an unprivileged test runner. AddressSanitizer/UndefinedBehaviorSanitizer builds can
be run with the same suite:

```sh
make clean
make test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-pthread -fsanitize=address,undefined -no-pie'
```

## Socket utilities

`send_all` retries interruptions and partial sends, suppressing SIGPIPE.
`recv_line` buffers unread bytes across calls, handles fragmented/coalesced
messages, and reports EOF, truncated EOF, socket errors, and oversized lines
separately. Its buffer capacity includes the trailing NUL; embedded NULs are
counted, so consumers must validate the returned length. Close the connection
after an error, truncated line, or oversized line. See `include/net_io.h`.

## Team Members & Task Assignments

These assignments cover the complete application, including planned work beyond
the current server core. Graded tasks and additional responsibilities are listed
separately for each member.

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
- Report failed download attempts to the server.
- Implement client-side tests and maintain client modules.

### Shared Responsibilities

Both members are responsible for:

- Following the agreed TCP communication protocol.
- Maintaining C11 compatibility with Ubuntu 20.04.
- Testing their assigned components.
- Reviewing GitHub pull requests.
- Integrating and testing the complete application.
- Debugging cross-component issues.
