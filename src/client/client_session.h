#ifndef P2P_CLIENT_SESSION_H
#define P2P_CLIENT_SESSION_H

#include "shared_index.h"

enum request_result { REQUEST_FAILED = -1, REQUEST_REJECTED, REQUEST_OK };
typedef struct ClientSession ClientSession;

/* Main-thread command interface. Exchanges are serialized with failure reports
 * and queued withdrawals. Do not free the session during an exchange. Commands
 * print their protocol/CLI status. REQUEST_OK means the operation completed;
 * REQUEST_REJECTED is a recoverable input/server/local-storage rejection;
 * REQUEST_FAILED is terminal (connection failure or requested cancellation).
 * A rejection can retain local intent as documented for share/unshare below.
 */

/* Borrow fd, index, downloads string, and optional signals until stop; the caller
 * keeps all of them alive. Returns an owned session or NULL/errno. The fd must be
 * connected and have I/O timeouts; this function never closes it on failure.
 */
ClientSession *client_session_start(int fd, uint32_t id,
                                    SharedIndex *index, const char *downloads, ClientSignals *signals);
/* Interrupt control I/O before joining peer workers that borrow the session. */
void client_session_interrupt(ClientSession *client);
/* Shut down (but do not close) fd, revoke serving/results, join the monitor,
 * and free the session. NULL is safe. Other borrowed resources remain owned by
 * the caller; destroy the index/signals only after all users have stopped.
 */
void client_session_stop(ClientSession *client);

/* Stage the client ID and listening port once per TCP connection. */
enum request_result client_stage_hello(ClientSession *client, uint16_t port);
/* The request is a complete REGISTER or LOGIN line, including CRLF. The
 * request buffer is wiped before a successful login republishes saved shares.
 */
enum request_result client_account(ClientSession *client, char *request, size_t capacity,
                                   bool registering);

/* Acknowledged logout or terminal control failure revokes serving/results;
 * a recoverable server rejection preserves them. The local catalog survives.
 */
enum request_result client_logout(ClientSession *client);

/* Persist local intent first. Offline or rejected announcements remain pending;
 * only an acknowledged SHARE enables serving. path may contain spaces.
 */
enum request_result client_share(ClientSession *client, const char *path);
/* Save the local removal, then withdraw its advertisement if online. A failed
 * save preserves the share. A rejected withdrawal is queued for retry.
 */
enum request_result client_unshare(ClientSession *client, const char *name);
/* Upload worker callback. Recheck potentially stale sources before reporting. */
void client_report_upload_failure(void *context, const char *name, bool check_stale);
/* Clear the previous selection, search an exact filename, and publish numbered
 * results after validation. Invalid/truncated responses fail the connection.
 */
enum request_result client_search(ClientSession *client, const char *query);
/* number selects the latest search. Peer failures are reported to the server.
 * A download never overwrites an existing file.
 */
enum request_result client_download(ClientSession *client, const char *number);
/* Print snapshots of desired local shares and their current announcement state. */
void client_shares(ClientSession *client);
/* Whether a transport/protocol failure was detected; does not consume it. */
bool client_session_failed(ClientSession *client);
/* Borrowed poll descriptor, valid until stop: never close or drain it. Becomes
 * readable on terminal control failure so blocked input can unwind.
 */
int client_session_failure_fd(ClientSession *client);

#endif
