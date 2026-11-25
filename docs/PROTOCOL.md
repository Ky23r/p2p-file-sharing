# TCP protocol

## Wire format

The control and peer channels use IPv4 TCP. Each wire text line ends with
CRLF (`\r\n`).

- Requests use `VERB [argument ...] CRLF`, with one ASCII space between tokens.
  The client sends uppercase verbs. Receivers accept any ASCII letter case
  for the verb and preserve argument case.
- A final reply uses `ddd SP text CRLF`, where `ddd` is a three-digit numeric
  code, `SP` is one ASCII space, and the nonempty printable ASCII text has no
  leading or trailing space. The code determines the outcome. Defined file
  replies also carry text that identifies the specific file condition.
- A successful search can have continuation lines in the form
  `200-<filename> <size> <client_id> <IPv4> <port> CRLF`. A final
  `200` line with a space after the code completes the reply.

The control server waits for the first request after accepting a connection.
It processes pipelined requests in order and sends each complete reply before
processing the next. TCP reads may split or coalesce any of these lines. Bare
LF, embedded CR, NUL, tabs, and other nonprintable bytes make a request invalid.
An empty line, leading space, invalid or unknown verb, or verb for the wrong
channel receives `500`. A recognized verb with missing, extra, or malformed
arguments receives `501`, including repeated or trailing spaces. Argument
validation precedes session checks. The control connection remains usable
after either response.

EOF discards a partial request. Socket errors and 30-second send or receive
inactivity timeouts can close a connection before a reply is sent. Line
buffers grow as needed.

## Control commands

| Request | Success | Session requirement |
| --- | --- | --- |
| `REGISTER <username> <password>` | `200 OK` | Logged out, stays logged out |
| `LOGIN <username> <password>` | `200 OK` | Logged out. Authenticates and activates a staged announcement |
| `HELLO <client_id> <port>` | `200 OK` | Any. Stages the announcement before login or updates it after login |
| `LOGOUT` | `200 OK` | Logged in, keeps the TCP connection open |
| `SHARE <client_id> <filename> <size>` | `200 OK` | Logged in with an active announcement matching the client ID |
| `UNSHARE <filename>` | `200 OK` | Logged in with an active announcement |
| `SEARCH <filename>` | Zero or more `200-` lines, then `200 OK` | Logged in with an active announcement |

## Reply codes

`2xx` indicates success, `4xx` a temporary failure, and `5xx` a rejection
that needs a corrected request, different session state, or available file.
The server and peers emit the following final reply lines.

| Reply | Meaning |
| --- | --- |
| `200 OK` | Control command completed, including the end of a search |
| `200 CHUNKED` | The peer accepted `GET` and will send framed file data |
| `421 Service unavailable` | The server could not start a control session or worker and closes the connection |
| `450 File temporarily unavailable` | The peer could not open the file due to a temporary error |
| `451 Local processing error` | Server storage, hashing, or allocation failed while processing the request |
| `500 Invalid or unknown command` | Invalid framing or verb, or a verb unsupported on this channel |
| `501 Invalid arguments` | A known command has invalid argument syntax, count, or values |
| `503 Bad command sequence` | A file command needs an active announcement, or `LOGIN` or `REGISTER` was sent while logged in |
| `530 Not logged in` | The command requires a login |
| `530 Invalid credentials` | An unknown username or incorrect password was supplied |
| `550 Account already exists` | The registration username is already in use |
| `550 Client ID in use` | Another authenticated connection holds the client ID |
| `550 Client ID does not match session` | The `SHARE` client ID differs from the active announcement |
| `550 File not shared` | The requester has no matching advertisement to unshare, or the peer has no active local share for `GET` |
| `550 File unavailable` | An advertised file is missing or inaccessible at its catalog path |

A rejected control request receives a single final reply. `421` ends the
connection. The client recognizes a valid final `200` by its code, even if
the server's reply text differs from `OK`.

The terminal accepts lowercase commands as listed in the [README](../README.md).
`help` displays the terminal command list and `quit` closes the connection
and exits. Neither sends a control command. The client and server also
accept `--help` at process launch.

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

A filename has 1–255 decoded bytes. ASCII control bytes, slash, backslash, and
the complete names `.` and `..` are forbidden. ASCII letters, digits, `-`, `.`,
`_`, and `~` appear literally. Other permitted bytes, including non-ASCII
bytes, use uppercase percent escapes. For example, `my notes.txt` becomes
`my%20notes.txt` and `báo cáo.txt` in UTF-8 becomes `b%C3%A1o%20c%C3%A1o.txt`.
Escaping a byte that can appear literally or using lowercase hex is invalid.

## Sessions and search

On connect, the client sends `HELLO` with its ID and listening port. The server
uses the source IPv4 address. Before login, the announcement is staged and
does not reserve the ID. Successful `LOGIN` activates it. If the ID is already
active on another connection, `LOGIN` returns `550 Client ID in use` and the
connection stays logged out. A connection that logs in without a staged
announcement can send `HELLO` afterward. An account may have several
connections with different active client IDs.

A rejected `HELLO` leaves the previous announcement intact. Updating the port
under the same ID preserves shares. Changing the ID clears shares. Logout
and disconnect release the ID and its shares atomically. Accounts persist
across server restarts. Announcements and the file index live in memory.

`SHARE` adds or updates the current connection's filename and size.
`UNSHARE` removes only that connection's advertisement. `SEARCH` matches
the complete filename, including case. Each continuation line carries the
size in bytes recorded at announcement. The downloaded size is the sum of
the frame payloads through the final frame.

The server sends a snapshot ordered by numeric client ID. The snapshot can
include the requester and may include a peer that disconnects after the lookup.
For example, each line below ends in CRLF.

```text
SEARCH my%20notes.txt
200-my%20notes.txt 2048 12 127.0.0.1 9001
200-my%20notes.txt 2048 34 127.0.0.1 9002
200 OK
```

An empty search returns just `200 OK`. A rejected search returns one error
reply with no continuation lines. The client makes results selectable only
after it receives the final `200` line. EOF, a different code, or a malformed
line after a continuation discards the accumulated results.

In the terminal, `search my notes.txt` displays the decoded filename, client
ID, IPv4 address, and port for each match. Each row starts with the number
used by `download NUMBER`. The example above appears as follows.

```text
1: my notes.txt (client 12, 127.0.0.1:9001)
2: my notes.txt (client 34, 127.0.0.1:9002)
200 OK
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

Each peer connection handles one request. The uploader returns a single
`500`, `501`, `450`, or `550` error line if it rejects the request before
sending a success header. A source read failure closes the connection before
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

If a `GET` finds an announced file missing or inaccessible, the
uploader returns the peer error and rechecks the file before sending
`UNSHARE` on its control connection. When the server confirms the listing
is absent, the local catalog entry stays pending for a later announcement.
Temporary failures leave the listing active. The downloader displays the
failure locally.

## Client and service state

The client refreshes its announcement every 10 seconds. A rejected refresh
preserves the previous announcement and allows later retries. Successful login
publishes readable saved shares. The terminal command `hello` refreshes the
announcement and retries saved shares when logged in. The terminal displays
`200 HELLO` on activation after login and on a successful manual refresh.
Missing or rejected shares remain pending in the local catalog. A client clears
search results after logout and before each new search. Malformed replies and
transport failure end the control session and disable new uploads.

A local share is saved before announcement. The terminal's `unshare` first
saves the catalog removal. A save failure leaves the local entry, serving
state, and server advertisement intact. A successful save disables new uploads
for that name. On an active session, the client then sends `UNSHARE` and treats
`200 OK` or `550 File not shared` as confirmation that the advertisement is
absent.

A rejected withdrawal stays queued in memory. The client retries it during the
10-second keepalive cycle or on a successful manual `hello`. Control exchanges
are serialized. A successful later `SHARE` for the same name cancels any queued
withdrawal for the older listing. Logout or disconnect clears any remaining
advertisements for the session. A saved removal survives a client restart,
including when the connection closed before the `UNSHARE` reply arrived.

The catalog uses hex-encoded names and absolute paths, separated by a tab
with one LF-terminated record per entry. A separate lock file protects writes.

The server logs UTC timestamps, connections, request statuses, successful
account usernames, peer announcements, file operations, and disconnects.
Passwords and raw malformed input are excluded. The log defaults to
`<accounts-path>.log` and can be set with `--log PATH`. It is a mode-0600
regular file. Log and socket writes occur outside the session mutex. An
unusable log prevents startup. Runtime log writes are best effort.
