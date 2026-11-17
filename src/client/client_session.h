#ifndef P2P_CLIENT_SESSION_H
#define P2P_CLIENT_SESSION_H

#include "shared_index.h"

enum request_result { REQUEST_FAILED = -1, REQUEST_REJECTED, REQUEST_OK };
typedef struct ClientSession ClientSession;

/* Main-thread command interface. Exchanges are serialized with the background
 * keepalive; do not call stop concurrently with a public operation. Commands
 * print their protocol/CLI status. REQUEST_OK means the operation completed;
 * REQUEST_REJECTED is a recoverable input/server/local-storage rejection;
 * REQUEST_FAILED is terminal (connection failure or requested cancellation).
 * A rejection can retain local intent as documented for share/unshare below.
 */

/* Borrow fd, index, downloads string, and optional signals until stop; the caller
 * keeps all of them alive. Returns an owned session or NULL/errno. The fd must be
 * connected and have I/O timeouts; this function never closes it on failure.
 */
ClientSession *client_session_start(int fd, uint32_t id, uint16_t port,
                                    SharedIndex *index, const char *downloads, ClientSignals *signals);
/* Shut down (but do not close) fd, revoke serving/results, join the keepalive,
 * and free the session. NULL is safe. Other borrowed resources remain owned by
 * the caller; destroy the index/signals only after all users have stopped.
 */
void client_session_stop(ClientSession *client);

/* request includes LF; expected excludes LF. One complete exchange; validates
 * success exactly and accepts well-formed ERR codes as recoverable rejections.
 */
enum request_result client_exchange(ClientSession *client, const char *request, const char *expected);

/* Announce peer and republish readable local shares. Individual SHARE rejections
 * leave those entries pending; a rejected HELLO preserves the prior state.
 */
enum request_result client_hello(ClientSession *client);
/* Acknowledged logout or terminal control failure revokes serving/results;
 * a recoverable server rejection preserves them. The local catalog survives.
 */
enum request_result client_logout(ClientSession *client);

/* Persist local intent first. Offline or rejected announcements remain pending;
 * only an acknowledged SHARE enables serving. path may contain spaces.
 */
enum request_result client_share(ClientSession *client, const char *path);
/* Acknowledge server removal before persisting local removal. A failed local
 * save retains an inactive entry: retry unshare before hello/login/restart.
 */
enum request_result client_unshare(ClientSession *client, const char *name);
/* Clear the previous selection, validate the entire bounded response, then
 * publish numbered results. Invalid/truncated responses fail the connection.
 */
enum request_result client_search(ClientSession *client, const char *query);
/* number selects the latest search. Report remote failures only; cancellation
 * and local destination errors never accuse the peer. Never overwrite a file.
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
