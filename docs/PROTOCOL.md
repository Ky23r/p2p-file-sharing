# TCP protocol

The server handles accounts, peer announcements, shared-file discovery, and
failed-download reports. The client manages the local catalog and transfers files
directly between peers.

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

## Client integration

The terminal client sends account and file commands, consumes complete counted
search responses, and lets the user select a numbered peer. Search matches the
complete, case-sensitive filename. Both control requests and peer GET requests
reuse the canonical filename tokens and uint64 byte sizes above. The client uses
the shared helpers in `include/protocol.h` for wire values.

The client repeats an acknowledged HELLO every 10 seconds, including while
terminal input is idle or a download is active. A rejected HELLO is displayed
and preserves the previous announcement and automatic retries. Invalid framing,
transport failure, SERVER_BUSY, and LINE_TOO_LONG disable new uploads and end
the control session. Explicit successful HELLO republishes readable local shares.
Missing or rejected files remain pending. Logout clears search results and serving
state while retaining the desired catalog for the next login.

Failed downloads use REPORT with the server's accepted reasons. A missing peer
file maps to NOT_FOUND. Malformed replies, premature EOF, and other transfer
failures map to TRANSFER_FAILED. Size and timeout failures retain their specific
reasons. Local destination failures and requested cancellation are not reported.
Reports about the client's own advertisement are skipped because the server
rejects self-reports. Server rejections are displayed without destroying an
otherwise usable control session.

## Peer transfer (Member 2)

The downloader connects to the selected result's IPv4/port and sends this request
using the canonical filename token:

```text
GET <filename>\n
```

The uploader accepts only a file in its local catalog that was successfully
announced on its currently active control session. A peer request does not carry
account credentials: announced files are available to any host that can reach the
listener. This is the controlled-network access policy, not peer authentication.

On success, send a header followed immediately by exactly `size` raw file bytes:

```text
OK FILE <size>\n
<size bytes, including arbitrary NUL/LF bytes>
```

Then close the connection. One file per connection; no pipelining, resume, or
trailer. Empty files have size 0 and no body. A downloader requires exact size
(also equal to the search result), followed by clean EOF: extra bytes, early EOF,
or a missing close are failures. Limit reads to fixed buffers, not file-size
allocations. Header readers may prefetch body bytes: all body reads must consume
the same `net_reader` buffer before returning to the socket.

Before a success header, errors are `ERR <CODE>\n` followed by connection close.
Peer codes are INVALID_REQUEST, UNKNOWN_COMMAND, LINE_TOO_LONG, NOT_SHARED,
FILE_UNAVAILABLE, FILE_CHANGED, and INTERNAL_ERROR. After a success header, an
upload error closes the connection; never insert an error line into file bytes.
Serving checks regular-file type and advertised size. Files must remain unchanged
while shared; same-size in-place edits are outside the integrity guarantee.

The listener runs four bounded upload workers with 30-second socket inactivity
timeouts. Unsharing/logout/control failure disables new opens; transfers that
already opened a file may finish. Process exit interrupts and joins all workers.
An unregistered or unauthenticated client never opens a new file for a requester.

Downloads use a mode-0600 temporary file in the explicitly configured download
directory. Publish the completed file only after size/EOF checks and fsync, without
overwriting an existing file or symlink. Failures remove only that temporary file.
Downloaded files are not automatically shared. Compare file contents in tests;
there is no content hash, TLS, NAT traversal, or peer identity proof in this version.

HUP/INT/QUIT/TERM/PIPE request orderly client shutdown. The main thread handles
these signals, interrupts its control/download sockets, and wakes terminal input;
workers inherit them blocked. Cancellation removes an unpublished download's
temporary file without reporting a peer failure, restores password echo, joins
workers, and exits with `128 + signal`. An already published complete file stays.
The signal controller blocks handled signals before capturing final exit status;
signals pending after that teardown boundary use the previous dispositions.

## Local client catalog and CLI

`--index PATH` defaults to `index.txt`; `--downloads DIRECTORY` defaults to `.`.
Use a separate config, index, download directory, and peer port for each client.
Catalog records are `name_hex<TAB>absolute_path_hex<LF>`, at most 256 records and
4095 decoded path bytes. A separate `.lock` file prevents concurrent writers.
Atomic replacements persist a complete catalog. Invalid existing data fails
startup unchanged. Previously saved non-ASCII basenames stay pending and can be
removed with `unshare`. Local directories and configuration must be trusted.

Commands are `share PATH`, `unshare NAME`, `shares`, `search NAME`, and
`download NUMBER`, in addition to existing account/session commands. File command
arguments use the literal remainder of the input line, allowing spaces.
The share name is the path's basename. A name already mapped to another path must
be unshared first. A local share may be saved before login; it remains pending
until successfully announced. A rejected SHARE retains that pending local entry.
Explicit HELLO retries pending shares. Logout clears search results and serving
state, while preserving the local catalog. Download numbers refer only to the
latest completely validated search response and are cleared before a new search.

SHARE persists local intent before sending to the server. For an active session,
the CLI treats either `OK UNSHARED` or `ERR FILE_NOT_FOUND` as confirmation that
the file is absent on the server. It then disables serving and saves the removal.
If that local save fails, retain an inactive pending entry, print the CLI-only
`ERR LOCAL_STORAGE` status and a detailed error:
retry `unshare` after fixing storage, before `hello`, login, or restart, because
those operations can republish the retained entry. Catalog saves use fsync and
atomic rename. A machine crash can still lose the updated directory entry.

Examples (all arrows below represent complete LF-terminated headers):

```text
Client -> server: SHARE report.pdf 5
Server -> client: OK SHARED
Client -> server: SEARCH report.pdf
Server -> client: OK RESULTS 1
Server -> client: FILE report.pdf 5 12345 127.0.0.1 9001
Downloader -> peer: GET report.pdf
Peer -> downloader: OK FILE 5
Peer -> downloader: [five raw bytes, then EOF]
```
