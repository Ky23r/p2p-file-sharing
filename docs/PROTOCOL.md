# Protocol

## Transport and framing

Control and peer connections use IPv4 TCP. Text lines end in CRLF
(`\r\n`). Requests use a command followed by its arguments, separated by
one ASCII space. Command verbs are case-insensitive. Arguments preserve case.

The control server handles pipelined requests in order. Control lines can
contain up to 65,536 bytes before LF, including CR. Malformed requests receive
`400`. For an oversized request, the server attempts `400` and closes the
connection.

An idle control connection waits for the next request. Both endpoints
use TCP keepalive with a 60-second idle interval, 10 seconds between
probes, and up to three unanswered probes. During an exchange, socket reads
and writes have a 30-second inactivity timeout. EOF discards a partial
request. A disconnect releases the session's client ID and advertisements.

## Control commands

| Request | Effect on success | Session requirement |
| --- | --- | --- |
| `HELLO <client_id> <port>` | Stage or update the peer address | Any |
| `REGISTER <username> <password>` | Create an account and keep the connection logged out | Logged out |
| `LOGIN <username> <password>` | Authenticate and activate the staged announcement, if present | Logged out |
| `LOGOUT` | Release active ID and shares and retain the staged announcement on this connection | Logged in |
| `SHARE <client_id> <filename>` | Advertise the filename for the current connection | Active announcement with matching ID |
| `UNSHARE <filename>` | Ensure this session's advertisement is absent | Active announcement |
| `SEARCH <filename>` | Return the number of matching peers and their endpoints | Active announcement |
| `FAIL <client_id> <filename> <code>` | Record a peer failure or withdraw the owner's stale advertisement | Active announcement |

The client sends one `HELLO` when it connects. The server records the source
IPv4 address of that connection. While logged out, a staged announcement
holds no reservation. `LOGIN` can return `409` if another connection holds
the staged ID. A wire client that logs in without an announcement can send
`HELLO` afterward. An update under the same ID preserves shares. A successful
change to a different ID clears the previous shares. A rejected update
preserves the current announcement.

The server stores active peer endpoints and advertisements in memory.
Accounts persist in the account database. The append-only service log uses
UTC timestamps and records connection events and request outcomes.

The client saves its random unsigned 32-bit ID in `config.txt`. It saves
basenames and absolute paths in `index.txt` as hex-encoded fields separated
by a tab, with one LF per record. The client publishes readable saved files
after login.

## Reply codes

Every ordinary reply consists of three digits followed by CRLF. A successful
`SEARCH` adds a count on its status line. A successful peer `GET` follows
its `200` with binary frames.

| Code | Meaning |
| --- | --- |
| `200` | Success. An `UNSHARE` for an absent advertisement also succeeds. |
| `400` | Invalid line, unknown or unsupported command, or invalid arguments. |
| `401` | Login required or invalid credentials. |
| `403` | The command requires an active announcement or a matching owner ID. |
| `404` | The requested peer file is unshared, missing, or inaccessible. |
| `409` | Account already exists, client ID is in use, or the account command requires a logged-out session. |
| `500` | Internal processing failure, such as account storage, hashing, or allocation. |
| `503` | Control session unavailable. The server closes the connection. On the peer channel, the source is temporarily unavailable. |

## Values

| Value | Representation |
| --- | --- |
| Username | ASCII letters, digits, or underscores, with at least one character |
| Password | At least eight printable ASCII characters, excluding space |
| Client ID | Unsigned decimal from 0 through 4294967295 |
| Peer port | Unsigned decimal from 1 through 65535 |
| Filename | Canonically encoded case-sensitive basename with 1–255 decoded bytes |

Unsigned decimal input accepts leading zeros and rejects signs and overflow.
Numeric output uses the shortest decimal representation. A filename can
include non-ASCII bytes. ASCII control bytes, slashes, backslashes, and
the complete names `.` and `..` are forbidden. ASCII letters, digits, `-`,
`.`, `_`, and `~` appear literally. Other permitted bytes use uppercase
`%HH` escapes.
For example, `my notes.txt` becomes `my%20notes.txt`. Escaping an unreserved
byte or using lowercase hex is invalid.

## Search

The server takes a snapshot of peers advertising the exact filename, excludes
the requester, and sorts the result by numeric client ID. The response starts
with `200 <count>` and has exactly `count` lines of `<client_id> <IPv4> <port>`.

```text
SEARCH my%20notes.txt
200 2
12 127.0.0.1 9001
34 127.0.0.1 9002
```

`200 0` is a complete empty response. A rejection is a single error status.
The next response starts after the last counted row. The client validates
the count and every row before displaying `Found <count>` and the numbered
choices for `download NUMBER`. A failed or interrupted search clears the
previous selection. Peers can disconnect after the snapshot.

## Peer transfer

The downloader connects to the selected endpoint and sends `GET <filename>`.
The uploader's four workers serve regular files with active advertisements.
Each peer connection handles one request. Peer text lines are limited to
771 bytes including CRLF. Peer sockets use 30-second inactivity timeouts.

A successful `GET` starts with `200\r\n`. Each frame is a header immediately
followed by the specified number of binary bytes. The tags include their
literal angle brackets.

| Frame header | Payload |
| --- | --- |
| `<data> <length>\r\n` | 1–65536 bytes followed by another frame |
| `<end> <length>\r\n` | 0–65536 bytes completing the file |

The following sequence transfers `abcde`.

```text
200\r\n<data> 3\r\nabc<end> 2\r\nde
```

An empty file uses `<end> 0\r\n`. Payload can contain any bytes. The uploader
reads through the source's EOF, so changes to a live source can affect the
bytes sent. A read or send failure can terminate the stream before its final
frame. The downloader writes to a private temporary file and publishes it
only after receiving the final frame and syncing the file. Publication
preserves any existing destination. The downloader closes after the final
payload.

### Failure notices

Peers with an active control session use `FAIL <client_id> <filename> <code>`.
The ID identifies the uploader and the server replies `200` to valid notices.

| Code | Trigger | Index action |
| --- | --- | --- |
| `503` | An unconfirmed peer failure, such as a refused connection, timeout, or interrupted transfer | Log the notice and retain the advertisement |
| `404` | The uploader rechecks the active source and confirms it is missing, inaccessible, or an invalid source type | Withdraw that filename only from the reporting owner's session |

A `404` notice requires the reporting session's own ID. A different ID
receives `403`. Repeating a valid notice after the advertisement is absent
succeeds. The owner keeps the local catalog entry pending after the server
acknowledges the notice. Destination errors and user cancellation stay local.

See [error handling](ERROR_HANDLING.md) for failure scenarios and recovery.
