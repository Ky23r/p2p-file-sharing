# TCP protocol

## Implementation boundary

The production server currently implements the account/session commands below.
The client implements the file-discovery extension and the peer-transfer protocol.
The extension is the integration contract for Member 1's remaining server work;
`tests/client/control_fixture.py` is a test fixture, not a production server.
Both members must review this contract before integrating the server extension.

## Common framing and values

Control connections and peer connections use IPv4 TCP. Headers are printable
ASCII with exactly one space between tokens, no leading/trailing spaces, and LF
termination. CRLF, tabs, NUL, other control bytes, and non-ASCII header bytes are
invalid. The maximum header is 4096 bytes excluding LF. Unknown error codes may
be displayed but never interpreted as success. Decimal integers have no sign;
leading zeros are accepted. Overflow is rejected before conversion.

File names are opaque filename bytes (normally UTF-8), encoded as lowercase hex
on the wire. Decoded names contain 1..255 bytes, no NUL, ASCII control bytes,
DEL, slash, or backslash, and cannot be `.` or `..`. This supports spaces and
non-ASCII names without changing the ASCII framing. Names are case-sensitive.
File sizes are decimal bytes from 0 through 1099511627776 (1 TiB). Local shares
and a search response are each limited to 256 entries. Paths are local only and
are never sent to another peer or the control server.

Each control request receives its entire response before another request starts.
Clients serialize complete exchanges, including background HELLO requests.
Socket inactivity timeouts are 30 seconds; these are not total-transfer deadlines.
After a framing error, oversized line, truncated response, or transport failure,
close the connection rather than attempting to resynchronize.

## Existing account/session protocol

| Request | Success | State |
| --- | --- | --- |
| `REGISTER <username> <password>` | `OK REGISTERED` | Logged out; remains logged out |
| `LOGIN <username> <password>` | `OK LOGGED_IN` | Logged out; authenticates this TCP connection |
| `HELLO <client_id> <port>` | `OK HELLO` | Requires login; announces a peer |
| `LOGOUT` | `OK LOGGED_OUT` | Requires login; clears the session, keeps TCP open |

Usernames contain 3..32 ASCII letters, digits, or underscores. Passwords contain
8..128 bytes in 0x21..0x7e. Client IDs are uint32 values, including zero; ports
are 1..65535. The server records the observed connection IP, not the ephemeral
source port. One account may use multiple connections, with independent sessions.
Only one active session may hold a given client ID. A failed HELLO preserves the
previous registration. LOGIN alone does not register a peer; the CLI sends HELLO
after login. LOGOUT/disconnect release the ID. Only accounts persist on the server.

Errors are `ERR <CODE>`. Existing codes are INVALID_REQUEST, UNKNOWN_COMMAND,
AUTH_REQUIRED, ALREADY_AUTHENTICATED, USER_EXISTS, INVALID_CREDENTIALS,
CLIENT_ID_IN_USE, LINE_TOO_LONG, SERVER_BUSY, and INTERNAL_ERROR. Syntax validation
precedes authorization. Malformed requests may be followed by more requests;
LINE_TOO_LONG and SERVER_BUSY close the connection. EOF discards unfinished
requests. Transport inactivity closes the connection without a response.

The client silently repeats its successful HELLO every 10 seconds to preserve an
idle authenticated connection, including during a long peer transfer. Repeating
the same ID/port is idempotent and must preserve the session's shared-file index.
A rejected background HELLO is reported on stderr and retains the previous
announcement and future automatic retries. Transport errors, invalid responses,
and the connection-closing SERVER_BUSY/LINE_TOO_LONG errors still disable serving.

## File-discovery extension (server implementation pending)

All operations require LOGIN and a successful HELLO. Errors include AUTH_REQUIRED,
PEER_REQUIRED, INVALID_REQUEST, NOT_SHARED, SHARE_LIMIT, RESULT_LIMIT, and
INTERNAL_ERROR, using the existing error framing.

| Request | Success | Meaning |
| --- | --- | --- |
| `SHARE <name_hex> <size>` | `OK SHARED` | Add/update this session's file metadata |
| `UNSHARE <name_hex>` | `OK UNSHARED` | Remove this session's file metadata |
| `SEARCH <query_hex>` | `OK RESULTS <count>` followed by result rows | Find shares |
| `REPORT_FAIL <client_id> <name_hex> <reason>` | `OK REPORTED` | Record a failed download |

SHARE is idempotent for the same name/size and updates size otherwise. File identity
is `(client_id, name_bytes)`; different peers may share the same name with different
contents. UNSHARE is idempotent, including an already absent name. The owner is the
requesting session, never an ID supplied in a SHARE/UNSHARE request.

SEARCH uses a nonempty filename query under the same encoding/limits as names.
Match a case-sensitive byte substring. Include only currently announced shares,
including the requester's own shares. Sort by decoded name bytes, then client ID.
Zero results is `OK RESULTS 0` with no rows. If there are more than 256 matches,
return `ERR RESULT_LIMIT` with no partial response. Each of exactly `count` rows is:

```text
FILE <client_id> <IPv4> <port> <size> <name_hex>\n
```

Do not add a terminator after the rows. Repeated `(client_id, name)` rows, invalid
fields, and counts over 256 are protocol errors. Clients select a numbered result;
they do not use untrusted file names as destination paths.

REPORT_FAIL reasons are CONNECT_FAILED, TIMEOUT, PEER_ERROR, INVALID_RESPONSE,
SIZE_MISMATCH, and INTERRUPTED. Identify a result previously returned to the
reporter's session. The server logs reporter, target, name, reason, and time;
one unverified report must not remove another peer's shares. Local destination
errors are shown locally and are not attributed to a remote peer. Reports receive
normal error responses if unsupported or no longer applicable.

LOGOUT/disconnect remove all of the session's shares. Changing an existing HELLO
ID or port clears that session's server shares; the client republishes its local
shares after an explicit successful HELLO. Failed HELLO changes nothing. The
client retains its local catalog across logout/restart and republishes readable
files after each new login/HELLO. Missing files stay local but are not advertised.
An explicit HELLO also withdraws previously announced files that can no longer be
opened. A rejected reannouncement disables local serving and attempts to withdraw
the previous advertisement. A rejected withdrawal is displayed; the server may
retain stale metadata until session cleanup.
Member 1 must extend the current two-argument request parser for REPORT_FAIL.

## Peer transfer (Member 2)

The downloader connects to the selected result's IPv4/port and sends:

```text
GET <name_hex>\n
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

## Local client catalog and CLI

`--index PATH` defaults to `index.txt`; `--downloads DIRECTORY` defaults to `.`.
Use a separate config, index, download directory, and peer port for each client.
Catalog records are `name_hex<TAB>absolute_path_hex<LF>`, at most 256 records and
4095 decoded path bytes. A separate `.lock` file prevents concurrent writers;
atomic replacements persist a complete catalog. Invalid existing data fails
startup unchanged. Local directories/configuration must be trusted.

Commands are `share PATH`, `unshare NAME`, `shares`, `search QUERY`, and
`download NUMBER`, in addition to existing account/session commands. PATH, NAME,
and QUERY are the literal remainder of the input line, allowing embedded spaces.
The share name is the path's basename. A name already mapped to another path must
be unshared first. A local share may be saved before login; it remains pending
until successfully announced. A rejected SHARE retains that pending local entry.
Explicit HELLO retries pending shares. Logout clears search results and serving
state, while preserving the local catalog. Download numbers refer only to the
latest completely validated search response and are cleared before a new search.

SHARE persists local intent before sending to the server. UNSHARE first obtains
the server acknowledgement, then disables serving and persists local removal.
If that local save fails, retain an inactive pending entry, print the CLI-only
`ERR LOCAL_STORAGE` status and a detailed error:
retry `unshare` after fixing storage, before `hello`, login, or restart, because
those operations can republish the retained entry. Catalog saves use fsync and
atomic rename; directory-entry durability across a machine crash is not promised.

Examples (all arrows below represent complete LF-terminated headers):

```text
Client -> server: SHARE 7265706f72742e706466 5
Server -> client: OK SHARED
Client -> server: SEARCH 7265706f7274
Server -> client: OK RESULTS 1
Server -> client: FILE 12345 127.0.0.1 9001 5 7265706f72742e706466
Downloader -> peer: GET 7265706f72742e706466
Peer -> downloader: OK FILE 5
Peer -> downloader: [five raw bytes, then EOF]
```
