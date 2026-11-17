#ifndef P2P_CLIENT_SESSION_H
#define P2P_CLIENT_SESSION_H

#include "shared_index.h"

enum request_result { REQUEST_FAILED = -1, REQUEST_REJECTED, REQUEST_OK };
typedef struct client_session client_session;

/* fd remains caller-owned; start/stop encompass the background keepalive. */
client_session *client_session_start(int fd, uint32_t id, uint16_t port,
                                     shared_index *index, const char *downloads);
void client_session_stop(client_session *client);
enum request_result client_exchange(client_session *client, const char *request, const char *expected);
enum request_result client_hello(client_session *client);
enum request_result client_logout(client_session *client);
enum request_result client_share(client_session *client, const char *path);
enum request_result client_unshare(client_session *client, const char *name);
enum request_result client_search(client_session *client, const char *query);
enum request_result client_download(client_session *client, const char *number);
void client_shares(client_session *client);
bool client_session_failed(client_session *client);
int client_session_failure_fd(client_session *client);

#endif
