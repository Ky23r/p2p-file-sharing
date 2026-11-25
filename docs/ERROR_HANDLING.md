# Error handling

See the [protocol](PROTOCOL.md) for wire syntax and numeric reply codes.

## Startup and persisted data

| Scenario | Detection | Handling |
| --- | --- | --- |
| Invalid address, port, or option | Argument parsing and IPv4 conversion fail | Print a diagnostic and exit before serving. |
| Address already bound or connection cannot be established | Socket setup, bind, connect, or connect timeout fails | Release allocated resources. Startup fails for the control connection; a selected peer connection failure is handled as a download failure. |
| Missing client ID file | Opening the ID file returns `ENOENT` | Generate a random 32-bit ID, sync the temporary record, and publish it without overwriting. Read the winning record if another instance created it first. |
| Malformed or unreadable client ID | File checks, read, or decimal validation fail | Exit and preserve the existing file. |
| Missing client catalog | Opening the catalog returns `ENOENT` | Start with an empty catalog under the catalog lock. |
| Invalid, unsafe, or locked catalog | File checks, exclusive lock, or record parsing fails | Exit and preserve existing catalog data. Source files are checked when published or served. |
| Unreadable, unsafe, locked, or malformed account store | Open, file checks, lock, or record decoding fails | The server fails startup and preserves account contents. |
| Invalid service-log destination | Open or file checks fail, or the log resolves to the account store | The server fails startup before writing log events. |
| Runtime log failure | Timestamp, memory allocation, or log write fails | Logging is best effort. Request handling proceeds. |

## Control connection and accounts

| Scenario | Detection | Handling |
| --- | --- | --- |
| Split or coalesced TCP input | Buffered reads see part of a line or bytes from later commands | Reassemble complete CRLF lines and process requests in order. |
| Malformed request | CRLF, ASCII framing, command, arity, numeric value, or filename validation fails | Reply `400` and continue with the next line. |
| Oversized request | More than 65,536 bytes precede LF | Attempt a `400` reply and close that connection. |
| Partial request, stalled reply, or socket failure | EOF, active I/O timeout, or send/receive fails | End the affected connection. The server clears its peer ID and advertisements. The client clears selections and disables new uploads after a control failure. |
| Idle or silently disconnected control connection | A socket reports failure after TCP keepalive probes | Healthy idle sessions stay open. A detected disconnect releases the active ID and advertisements. |
| Capacity or worker startup failure | The server cannot allocate or start a session | Attempt a `503` reply and close the incoming socket. |
| Commands before login or incorrect credentials | Session state or password verification check fails | Reply `401`. Unknown accounts and wrong passwords use the same response. |
| Missing active announcement or owner mismatch | Session or client ID check fails | Reply `403` and preserve other owners' shares. |
| Duplicate account, claimed client ID, or account command while logged in | Account lookup or session state check reports a conflict | Reply `409`. A rejected login or announcement leaves previous valid state intact. |
| Hashing, allocation, or account write failure | Credential preparation, allocation, or account append or sync fails | Reply `500`. A new account becomes available after its record is synced. A failed append is rolled back. A failed rollback disables further account operations. |
| Invalid or incomplete control response | Status, framing, or expected search count/rows fail validation | The client rejects incomplete data, clears selections, and ends the control session. |

## Sharing and discovery

| Scenario | Detection | Handling |
| --- | --- | --- |
| Invalid local source or same basename at a different path | Path resolution, filename validation, regular-file check, or catalog lookup fails | Reject the share. Keep any existing entry intact. |
| Catalog save fails during share or unshare | Temporary file creation, write, sync, or atomic rename fails | Preserve the previous catalog and its serving state. Show a local error. |
| Share saved without an active session, or first publication rejected | The server has no successful `SHARE` for that entry | Keep the file pending. A subsequent login or `share PATH` can publish it. |
| Re-publication rejected for an already advertised readable file | The server rejects `SHARE` while the local source is still valid | Preserve the current advertisement and upload access. |
| Server rejects withdrawal after local removal | The catalog save succeeded and `UNSHARE` returned a recoverable error | Stop new uploads and retry the queued withdrawal about every 10 seconds while connected. A successful share of the same name cancels it. Disconnect clears the advertisement. |
| Withdrawal reply is lost | Control response read fails after the catalog was saved | End the failed connection. The saved removal survives restart, so the share stays absent on the next login. |
| Empty search or stale selection | Count is zero, or selected index is outside the latest result list | Display `Found 0` for zero matches and reject invalid download selections. The old selection is cleared when a new search starts or the session ends. |
| Search interrupted during a snapshot | A row is missing, malformed, duplicated, out of order, or refers to the requester | Reject the whole response and close the control connection. No partial list becomes selectable. |

## Peer transfers and shutdown

| Scenario | Detection | Handling |
| --- | --- | --- |
| Invalid `GET` or unavailable source | Request validation fails, or the requested file cannot be opened from the active catalog | Reply `400` for an invalid request, `404` for an unshared or stale file, or `503` for a temporary open error. Close the peer socket. |
| Source confirmed stale | The owner reopens the advertised path and confirms a permanent source error such as a missing file or final symlink | Send `FAIL` with `404`. The server withdraws only the owner's advertisement and the client keeps the catalog entry pending. |
| Refused connection, timeout, read or send error, or broken framing | Transport fails or the downloader sees an invalid status, malformed frame, or EOF before a complete `<end>` frame | Abort the transfer and clean up the partial file. An active control session reports the unconfirmed failure as `FAIL` with `503`. The server keeps the advertisement. |
| Existing destination, local write failure, or final-name race | Destination check, temporary file creation, write, sync, or atomic publication fails | Preserve existing files. Remove this attempt's temporary file when possible. Destination failures stay local. |
| Source changes during upload | The uploader encounters EOF on the open file, which can grow or shrink during a transfer | The final frame reflects the bytes read from the open source. The content of a growing or changing file can differ from its earlier contents. |
| Cancellation, termination signal, or normal quit | The client handles a signal or exits its terminal loop | Interrupt sockets, restore password echo when applicable, discard incomplete downloads, and join workers. A file already published is retained. The server clears the session on disconnect. |
| Server shutdown | The server receives a shutdown signal | Shut down active sockets, join session workers, and release in-memory announcements and shares. Persisted account records remain available for restart. |

Temporary-file cleanup is best effort if the filesystem refuses removal. A
forced process kill interrupts cleanup and can leave temporary artifacts.
