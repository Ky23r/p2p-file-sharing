# TCP protocol

## Framing

The control and peer protocols use IPv4 TCP. Every request, status, and result
line ends in CRLF (`\r\n`). Text is printable ASCII. Commands are
case-sensitive, and a single ASCII space separates tokens. Empty lines, bare
LF, embedded CR, NUL, tabs, and leading, trailing, or repeated spaces are invalid.

A control connection processes requests in order, including when a TCP read
contains part of a line or several lines. Malformed requests receive `400` and
the connection can continue. EOF discards a partial request. Socket errors and
30-second send or receive inactivity timeouts can close the connection without
a reply. Line buffers grow as needed. The inactivity timeout applies between
socket operations.

## Control commands

| Request | Success | Session requirement |
| --- | --- | --- |
| `REGISTER <username> <password>` | `201` | Logged out, stays logged out |
| `LOGIN <username> <password>` | `200` | Logged out, authenticates this connection |
| `HELLO <client_id> <port>` | `200` | Logged in |
| `LOGOUT` | `200` | Logged in, keeps the TCP connection open |
| `SHARE <filename> <size>` | `200` | Logged in with a successful HELLO |
| `UNSHARE <filename>` | `200` | Logged in with a successful HELLO |
| `SEARCH <filename>` | Zero or more `FILE` lines, then `200 END` | Logged in with a successful HELLO |

| Status | Meaning |
| --- | --- |
| `200` | Command completed. `200 END` ends a successful search. A peer transfer starts with `200 CHUNKED`. |
| `201` | Account created. |
| `400` | Invalid framing, syntax, argument count, or argument value. |
| `401` | Login required, or the supplied login credentials are invalid. Unknown users and wrong passwords receive the same reply. |
| `403` | Complete HELLO before issuing file commands. |
| `404` | UNSHARE could not find an advertisement owned by this connection, or the requested peer file is unavailable. |
| `409` | Conflict. The account already exists, the connection is already logged in, or another connection holds the client ID. |
| `500` | Internal failure, such as an account storage, hashing, or allocation error while processing the request. |
| `501` | Unrecognized command in a syntactically valid request. |
| `503` | The server could not start a session or worker and closes the connection. |

A control error is a single line containing the code. Syntax and value
validation precede session checks. A command with more than two arguments
receives `400`.

## Values

| Value | Representation |
| --- | --- |
| Username | One or more ASCII letters, digits, or underscores, case-sensitive |
| Password | At least eight bytes from `!` through `~`, excluding spaces, and within the OpenSSL `int` length limit |
| Client ID | Unsigned decimal, 0 through 4294967295 |
| Peer port | Unsigned decimal, 1 through 65535 |
| File size | Unsigned decimal, 0 through 18446744073709551615 bytes |
| Filename | Canonically encoded, case-sensitive basename |

Decimal inputs accept leading zeros and reject signs and overflow. Numeric
output uses the shortest decimal representation.

A filename has 1–255 decoded bytes from ASCII space through `~`. Slash,
backslash, and the complete names `.` and `..` are forbidden. Letters, digits,
`-`, `.`, `_`, and `~` appear literally. Other permitted bytes use uppercase
percent escapes. For example, `my notes.txt` becomes `my%20notes.txt` and
`100%.txt` becomes `100%25.txt`. Escaping a byte that can appear literally or
using lowercase hex is invalid.

## Sessions and search

HELLO stores the source IPv4 address, client ID, and listening port. An active
client ID belongs to one control connection. An account may have several
connections. A rejected HELLO leaves the previous registration intact.
Updating the port under the same ID preserves shares. Changing the ID clears
shares. Logout or disconnect releases the ID and its shares atomically.
Accounts persist across server restarts. Peer and share state is in memory.

SHARE adds or updates the current connection's filename and size. UNSHARE
removes only that connection's advertisement. SEARCH matches the complete
filename, including case. Each `FILE` response carries the size in bytes
recorded at announcement. The actual download size is the sum of its payloads
through the final frame.

```text
FILE <filename> <size> <client_id> <IPv4> <port>
```

The server sends a snapshot ordered by numeric client ID. The snapshot can
include the requester and may include a peer that disconnects after the lookup.
For example, each line below ends in CRLF.

```text
SEARCH my%20notes.txt
FILE my%20notes.txt 2048 12 127.0.0.1 9001
FILE my%20notes.txt 2048 34 127.0.0.1 9002
200 END
```

An empty search returns `200 END` on the first line. A rejected search returns
its error code on the first line. The client consumes and validates the entire
reply before it makes any result selectable. EOF before `200 END` discards
the entire reply.

In the terminal, `search my notes.txt` displays the decoded filename, client
ID, IPv4 address, and port for each match. Each row starts with the number
used by `download NUMBER`. The example above appears as follows.

```text
1: my notes.txt (client 12, 127.0.0.1:9001)
2: my notes.txt (client 34, 127.0.0.1:9002)
200 END
```

## Peer transfer

The downloader connects directly to the selected peer and sends
`GET <filename>\r\n`. The uploader serves files successfully announced on its
active control session. A host that can reach the peer listener can request
announced files, so use the service on a controlled network.

A successful peer response starts with `200 CHUNKED\r\n`. The length of the
transfer is determined as the uploader reads the file. Update both peers
together to use this format.

| Frame header | Payload |
| --- | --- |
| `<data> <length>\r\n` | Exactly `length` bytes followed by another frame |
| `<end> <length>\r\n` | Exactly `length` bytes that complete the file |

The angle brackets in `<data>` and `<end>` are literal. Lengths are unsigned
decimal byte counts up to 65536. A `<data>` frame carries at least one byte.
An empty file has one `<end> 0\r\n` frame. Payload bytes immediately follow
each header and can include any byte sequence, including CRLF and the frame
tags. The next frame starts immediately after the preceding payload. Peer
text lines are limited to 771 bytes including CRLF.

For example, the following byte sequence sends the five bytes `abcde`.

```text
200 CHUNKED\r\n<data> 3\r\nabc<end> 2\r\nde
```

The downloader reads headers and payload through the same buffered reader.
It writes only payload bytes to a private temporary file and accepts the
transfer after reading a complete `<end>` frame. The uploader closes after the
last frame. The downloader finishes as soon as the final payload is complete
and discards any subsequent bytes when closing the connection. Socket EOF
before the final frame is complete or invalid framing fails the download and
deletes the temporary file. A completed download is published without
overwriting an existing path.

Before the success header the uploader may return `400`, `404`, or `501`
and close the connection. A source read failure closes the connection before
the final frame.

The peer listener uses four upload workers and 30-second socket inactivity
timeouts. Unshare, logout, or control failure disable new opens. An existing
upload can finish. The uploader reads an open regular file in order and marks
its last chunk when a read reaches EOF. Bytes appended before that EOF can be
included. Truncation can bring EOF forward. The stream retains bytes already
read, including when the source later overwrites or removes them. Keep the
source stable when a consistent snapshot is required.

The framing uses per-chunk lengths and an explicit terminator, as in HTTP/1.1
chunked transfer coding. The final frame can also carry payload, as in FTP
block mode with an EOF flag.
[RFC 9112 §7.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1) ·
[RFC 959 §3.4.2](https://www.rfc-editor.org/rfc/rfc959.html#page-22)

## Client and service state

The client refreshes a successful HELLO every 10 seconds. A rejected refresh
preserves the previous registration and allows later retries. Successful
explicit HELLO reannounces readable shares. Missing or rejected shares remain
pending in the local catalog. A client clears search results after logout and
before each new search. Malformed replies and transport failure end the control
session and disable new uploads.

A local share is saved before announcement. For an active session, either
`200` or `404` from UNSHARE confirms the advertisement is absent. If saving
the removal fails, the entry stays pending. Retry the removal after fixing
storage and before HELLO, login, or restart, which can reannounce it.
The catalog uses hex-encoded names and absolute paths, separated by a tab
with one LF-terminated record per entry. A separate lock file protects writes.
Legacy non-ASCII basenames can be removed from the catalog.

The server logs UTC timestamps, connections, request statuses, successful
account usernames, peer announcements, file operations, and disconnects.
Passwords and raw malformed input are excluded. The log defaults to
`<accounts-path>.log` and can be set with `--log PATH`. It is a mode-0600
regular file. Log and socket writes occur outside the session mutex. An
unusable log prevents startup. Runtime log writes are best effort.
