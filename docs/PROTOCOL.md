# TCP Control Protocol

This is the implemented client-to-server contract. The server handles accounts,
peer announcements, shared-file discovery, and failed-download reports. Direct
peer file transfer and the client file commands remain Member 2's work.

## Transport and framing

- IPv4 TCP, ASCII, case-sensitive commands, LF terminators only.
- Each request and each response line has at most 4096 bytes excluding LF.
- Exactly one ASCII space separates tokens. Leading/trailing spaces, repeated
  spaces, CR/CRLF, tabs, NUL, non-ASCII bytes, and other control bytes are invalid.
- Each request receives one response before the next response starts. `SEARCH`
  has a counted multiline response; other commands have a single response line.
- Fragmented TCP reads and multiple requests in one TCP write are supported.
  A client must consume every search result before interpreting another response.
- Invalid requests receive `ERR INVALID_REQUEST` and the connection remains open.
  Requests exceeding the line limit receive `ERR LINE_TOO_LONG` and close.
  EOF discards incomplete requests. Socket errors and inactivity timeouts close
  without a guaranteed response. The server uses 30-second send/receive inactivity
  timeouts; these do not bound the total lifetime of a slowly transmitted request.
- `ERR SERVER_BUSY` closes the connection when connection/thread capacity is full.
  A truncated search response must be discarded; EOF is never a result terminator.

## Values

| Value | Representation |
| --- | --- |
| Username | 3–32 ASCII letters, digits, or underscores; case-sensitive |
| Password | 8–128 ASCII bytes from `!` through `~`; no spaces |
| Client ID | Unsigned decimal, 0–4294967295 |
| Peer port | Unsigned decimal, 1–65535 |
| File size | Unsigned decimal, 0–18446744073709551615, in bytes |
| Filename | Canonically encoded printable ASCII basename, described below |
| Failure reason | `CONNECT_FAILED`, `NOT_FOUND`, `TIMEOUT`, `TRANSFER_FAILED`, or `SIZE_MISMATCH` |

Decimal inputs accept leading zeros and reject signs, missing digits, and
overflow. Numeric output has no leading zeros except the value zero.

### Filename encoding

A filename contains 1–255 decoded bytes from ASCII space through `~`. `/` and
backslash are forbidden; `.` and `..` are forbidden as complete names. Names are
case-sensitive. These are names advertised by peers, never server filesystem paths.

ASCII letters, digits, `-`, `.`, `_`, and `~` appear literally. All other permitted
bytes use `%HH` with uppercase hexadecimal digits. Encoding an unreserved byte,
using lowercase hex, or using an invalid escape is rejected. This gives each name
exactly one wire representation, at most 765 bytes long. For example:

| Local name | Wire token |
| --- | --- |
| `notes.txt` | `notes.txt` |
| `my notes.txt` | `my%20notes.txt` |
| `100%.txt` | `100%25.txt` |
| `a+b.txt` | `a%2Bb.txt` |

`protocol_encode_filename`, `protocol_decode_filename`, and
`protocol_parse_uint64` in `include/protocol.h` are shared helpers for clients.

## Commands and session state

| Request | Success | Required state |
| --- | --- | --- |
| `REGISTER <username> <password>` | `OK REGISTERED` | Logged out; remains logged out |
| `LOGIN <username> <password>` | `OK LOGGED_IN` | Logged out; authenticates this TCP connection |
| `HELLO <client_id> <port>` | `OK HELLO` | Logged in |
| `LOGOUT` | `OK LOGGED_OUT` | Logged in; TCP remains open |
| `SHARE <filename> <size>` | `OK SHARED` | Logged in and announced with `HELLO` |
| `UNSHARE <filename>` | `OK UNSHARED` | Logged in and announced with `HELLO` |
| `SEARCH <filename>` | Counted result response below | Logged in and announced with `HELLO` |
| `REPORT <client_id> <filename> <reason>` | `OK REPORTED` | Logged in and announced with `HELLO` |

Syntax and value validation happen before state checks. File commands return
`AUTH_REQUIRED` when logged out, then `HELLO_REQUIRED` when logged in but not
announced. Unknown commands allow at most two arguments, preserving the original
protocol; unknown commands with more arguments receive `INVALID_REQUEST`.

### Peer announcements and index lifetime

`HELLO` records the IP observed on the control connection and the announced port.
An active client ID is unique across connections. One account may have multiple
connections, each with its own peer identity and shares. A rejected `HELLO` leaves
the previous identity, endpoint, and shares intact.

Repeating the same ID preserves shares. Changing only the port updates the
endpoint returned by future searches. Successfully changing the ID clears that
connection's shares; the client must register them again under the new identity.
Logout and disconnect atomically release the ID and remove all of that connection's
shares. Accounts persist; peer information and shares do not survive a restart.
After a new login, send `HELLO` and then register local shares again.

### Shared-file registration and removal

`SHARE` advertises a filename and size for the current connection. Registering an
existing name updates its size, including when the connection already has 128
distinct shares. Each connection can advertise at most 128 distinct names;
another new name returns `SHARE_LIMIT`. Different peers can advertise the same
name. Sizes are peer-supplied metadata; the server does not inspect file contents.

`UNSHARE` removes only the requesting connection's advertisement. An unknown name,
including a name held only by another peer, returns `FILE_NOT_FOUND`.

### File search

Search is an exact, case-sensitive filename match, with no wildcard or substring
syntax. The response is:

```text
OK RESULTS <count>
FILE <filename> <size> <client_id> <IPv4> <port>
... exactly count FILE lines ...
```

Results are a consistent snapshot sorted by numeric client ID, with at most 128
records (one per active connection). They include the requester if it advertises
that name. Zero matches is `OK RESULTS 0` followed by no result lines. There is no
extra terminator. Peers may disconnect or change files after the snapshot.

Example (every line ends in LF):

```text
SEARCH my%20notes.txt
OK RESULTS 2
FILE my%20notes.txt 2048 12 127.0.0.1 9001
FILE my%20notes.txt 2048 34 127.0.0.1 9002
```

### Failed-download reports

`REPORT` identifies the peer, filename, and failure reason. The target must still
advertise the filename; otherwise the response is `FILE_NOT_FOUND`. Reporting
one's own client ID returns `INVALID_REQUEST`. Reports are advisory and recorded
in the service log. They do not remove advertisements or disconnect the target:
a report is not proof that the target's file is unavailable. If a client ID has
been released and reused, the report refers to the current advertisement only.

```text
REPORT 34 my%20notes.txt TIMEOUT
OK REPORTED
```

## Errors

All errors are one LF-terminated line, `ERR <ERROR_CODE>`.

| Code | Meaning |
| --- | --- |
| `INVALID_REQUEST` | Invalid bytes, spacing, argument count/value, filename encoding, or self-report |
| `UNKNOWN_COMMAND` | Unrecognized command in an otherwise valid request |
| `AUTH_REQUIRED` | Operation requires login |
| `HELLO_REQUIRED` | File operation requires a successful peer announcement |
| `ALREADY_AUTHENTICATED` | `REGISTER` or `LOGIN` while logged in |
| `USER_EXISTS` | Username already exists |
| `INVALID_CREDENTIALS` | Unknown username or incorrect password |
| `CLIENT_ID_IN_USE` | Another active connection holds the client ID |
| `FILE_NOT_FOUND` | No advertisement owned by the relevant connection/target |
| `SHARE_LIMIT` | Connection already advertises 128 distinct filenames |
| `LINE_TOO_LONG` | Request exceeds 4096 bytes; connection closes |
| `SERVER_BUSY` | Connection/thread capacity unavailable; connection closes |
| `INTERNAL_ERROR` | Account storage, hashing, or allocation failure |

## Service logging

The default log is `<accounts-path>.log`; `--log PATH` overrides it. The server
appends UTC timestamped connection, request/result, disconnect, and lifecycle
events. Accepted file operations include encoded filenames; accepted reports also
include target ID and reason. Passwords and raw malformed requests are never
logged. File and session state share one mutex; log writes and socket writes
happen outside that mutex.

Log storage must be a regular file owned by the current user in a trusted parent
directory; it is restricted to mode 0600. An unusable log fails startup. Runtime
log write failures are best effort and do not change command results. Log data
is append-only during normal operation; rotation is an operator responsibility.

## Integration boundary for Member 2

The terminal client currently implements account commands and `HELLO` only.
Member 2 must implement local `index.txt`, file commands, full counted-search
parsing, and peer upload/download. That member owns the exact peer request and
response grammar; no peer-transfer commands are implemented by this milestone.

Peer transfer should reuse these canonical filename tokens and uint64 byte sizes.
Its specification must define headers, error responses, exact file byte counts,
timeouts, and connection closure, including zero-byte and binary files. If using
`net_reader` for headers, consume already buffered bytes before reading further
file data from the socket. Resolve requests only through the local shared-file
index. Control-server search results provide peer-supplied discovery metadata;
they do not guarantee file availability or integrity.

Plaintext authentication remains suitable only for controlled local testing.
