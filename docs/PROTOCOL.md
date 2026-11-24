# TCP protocol

## Wire format

The project uses its own IPv4 TCP protocol with command and reply conventions
from [FTP, RFC 959](https://www.rfc-editor.org/rfc/rfc959.html) and
[SMTP, RFC 5321](https://www.rfc-editor.org/rfc/rfc5321.html). In the examples,
each wire text line ends with CRLF (`\r\n`).

- Requests use `VERB [argument ...] CRLF`, with one ASCII space between tokens.
  The client sends uppercase verbs. Receivers accept any ASCII letter case
  for the verb and preserve argument case.
- A final reply uses `ddd SP text CRLF`, where `ddd` is a three-digit numeric
  code, `SP` is one ASCII space, and the nonempty printable ASCII text has no
  leading or trailing space. The code determines the outcome. Defined file
  replies also carry text that identifies the specific file condition.
- A successful search can have continuation lines in the form
  `200-<filename> <size> <client_id> <IPv4> <port> <revision> CRLF`. A final
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
| `REPORT <peer_id> <filename> <revision> <reason>` | `200 OK` | Logged in with an active announcement |

## Reply codes

`2xx` indicates success, `4xx` a temporary failure, and `5xx` a rejection
that needs a corrected request, different session state, or available file.
The server and peers emit the following final reply lines.

| Reply | Meaning |
| --- | --- |
| `200 OK` | Control command completed, including the end of a search |
| `200 <size>` | The peer accepted `GET` and will send `size` bytes |
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
| `550 File size changed` | The current file size differs from the peer's advertised size |

A rejected control request receives a single final reply. `421` ends the
connection. The client recognizes a valid final `200` by its code, even if
the server's reply text differs from `OK`. Every peer connection closes after
its one response.

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
| Advertisement revision | Unsigned decimal, 1 through 18446744073709551615 |
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
under the same ID preserves shares and assigns them a new revision. Changing
the ID clears shares. Logout or disconnect releases the ID and its shares
atomically. Accounts persist across server restarts. Announcements and the file
index are held in memory.

`SHARE` adds or updates the current connection's filename and size and assigns
a new revision even when the size stays the same. `UNSHARE` removes only that
connection's advertisement. `SEARCH` matches the complete filename, including
case. Each continuation line carries the size in bytes for transfer validation
and the revision to use in a failure report.

```text
200-<filename> <size> <client_id> <IPv4> <port> <revision>
```

The server sends a snapshot ordered by numeric client ID. The snapshot can
include the requester and may include a peer that disconnects after the lookup.
For example, each line below ends in CRLF.

```text
SEARCH my%20notes.txt
200-my%20notes.txt 2048 12 127.0.0.1 9001 5
200-my%20notes.txt 2048 34 127.0.0.1 9002 7
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

A successful peer response starts with `200 <size>\r\n`, followed by exactly
`size` raw bytes and a TCP close. An empty file has no body. The uploader
returns a single `500`, `501`, `450`, or `550` error line if it rejects the
request before sending a success header. A failure during the body closes the
connection. The downloader requires the advertised size and clean EOF. It
reads the header and body through the same socket buffer and writes to a
private temporary file. It publishes the complete file without overwriting
an existing path and deletes the temporary file on failure.

The peer listener uses four upload workers and 30-second socket inactivity
timeouts. Unshare, logout, or control failure disable new opens. An existing
upload can finish. The transfer checks file type and size. Shared files must
stay unchanged during transfers because the protocol carries no content hash.

## Download failure reports

The downloader sends `REPORT` on the control connection using the peer ID,
filename, and revision from the selected search result. Local destination
errors and cancellation stay local.

| Reason | When sent | Server action |
| --- | --- | --- |
| `NOT_FOUND` | Peer returned `550 File unavailable` | Withdraw the matching advertisement |
| `SIZE_CHANGED` | Peer returned `550 File size changed` | Withdraw the matching advertisement |
| `CONNECT_FAILED` | Connection to the peer failed | Record the failure |
| `TIMEOUT` | A peer socket operation timed out | Record the failure |
| `TRANSFER_FAILED` | The peer returned another error or sent an invalid or incomplete transfer | Record the failure |

The server checks the peer ID, filename, and revision together under the same
lock used for index changes. A duplicate or outdated report receives `200 OK`
and leaves any newer advertisement intact. Withdrawal affects just the matching
peer's advertisement. On this controlled network, the server relies on the
authenticated downloader to relay the peer's response accurately.

## Client and service state

The client refreshes its announcement every 10 seconds. A rejected refresh
preserves the previous announcement and allows later retries. Successful login
publishes readable saved shares. The terminal command `hello` refreshes the
announcement and retries saved shares when logged in. The terminal displays
`200 HELLO` on activation after login and on a successful manual refresh.
Missing or rejected shares remain pending in the local catalog. A client clears
search results after logout and before each new search. Malformed replies and
transport failure end the control session and disable new uploads.

A local share is saved before announcement. For an active session, either
`200 OK` or `550 File not shared` from `UNSHARE` confirms the advertisement
is absent. If saving the removal fails, the entry stays pending. Retry the
removal after fixing storage and before `hello`, login, or restart, which can
publish it again.
The catalog uses hex-encoded names and absolute paths, separated by a tab
with one LF-terminated record per entry. A separate lock file protects writes.

The server logs UTC timestamps, connections, request statuses, successful
account usernames, peer announcements, file operations, and disconnects.
Failure reports include the peer ID, filename, revision, reason, and action.
Passwords and raw malformed input are excluded. The log defaults to
`<accounts-path>.log` and can be set with `--log PATH`. It is a mode-0600
regular file. Log and socket writes occur outside the session mutex. An
unusable log prevents startup. Runtime log writes are best effort.
