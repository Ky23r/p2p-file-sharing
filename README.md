# p2p-file-sharing

C11 project targeting Ubuntu 20.04, GCC, POSIX TCP sockets, and pthreads.
The foundation contains reusable blocking socket I/O and an extensible server
entry point. File search, indexing, sharing, and transfers are outside this scope.

Install `build-essential libssl-dev python3`, then run:

```sh
make
make test
./server --help
make clean
```

The build uses `-std=c11 -Wall -Wextra -Wpedantic -pthread` and links `libcrypto`.
Generated files and runtime account storage are ignored by Git.

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

Errors: `INVALID_REQUEST` (bad syntax/arguments), `UNKNOWN_COMMAND`,
`AUTH_REQUIRED`, `ALREADY_AUTHENTICATED`, `USER_EXISTS`, `INVALID_CREDENTIALS`,
`CLIENT_ID_IN_USE`, `LINE_TOO_LONG`, `SERVER_BUSY`, and `INTERNAL_ERROR`.
Syntax validation precedes authorization checks. Unknown command names return
`UNKNOWN_COMMAND`. Malformed lines can be followed by more commands. Oversized
lines return `LINE_TOO_LONG` and close the connection; EOF discards incomplete
requests. Disconnects clear all session state. Accounts survive restarts.

The protocol above is the contract for the next server milestone; the current
entry point only displays usage. Plaintext TCP authentication is acceptable only
for controlled local testing. External deployment requires encrypted transport.

## Socket utilities

`send_all` retries interruptions and partial sends, suppressing SIGPIPE.
`recv_line` buffers unread bytes across calls, handles fragmented/coalesced
messages, and reports EOF, truncated EOF, socket errors, and oversized lines
separately. Its buffer capacity includes the trailing NUL; embedded NULs are
counted, so consumers must validate the returned length. Close the connection
after an error, truncated line, or oversized line. See `include/net_io.h`.
