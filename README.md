# p2p-file-sharing

C11 project targeting Ubuntu 20.04, GCC, POSIX TCP sockets, and pthreads.
The centralized control server provides registration, authentication, and
connection-local sessions. File search, indexing, sharing, and transfers are
outside this scope.

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

`make test` runs the C socket tests and Python standard-library TCP integration
tests. Each server test uses temporary data and available local ports. Tests cover
concurrent accounts/sessions, persistence and hash verification, malformed input,
storage failures, client limits, descriptor/thread cleanup, and graceful shutdown.
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
