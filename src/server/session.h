#ifndef P2P_SESSION_H
#define P2P_SESSION_H

#include "protocol.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_LIMIT PROTOCOL_PEER_LIMIT

typedef struct shared_file shared_file;

typedef struct session {
    int fd;
    char username[USERNAME_MAX + 1];
    bool authenticated;
    uint32_t client_id;
    uint16_t port;
    char ip[INET_ADDRSTRLEN];
    bool peer_registered;
    shared_file *files;
    size_t file_count;
    pthread_t thread;
    bool finished;
    struct session *next;
} session;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t empty;
    session *head;
    size_t count;
    bool stopping;
} session_manager;

typedef struct {
    char filename[PROTOCOL_FILENAME_TOKEN_MAX + 1];
    uint64_t size;
    uint32_t client_id;
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
} file_match;

enum session_result {
    SESSION_OK, SESSION_UNAUTHORIZED, SESSION_ID_IN_USE, SESSION_HELLO_REQUIRED,
    SESSION_FILE_NOT_FOUND, SESSION_SHARE_LIMIT, SESSION_INVALID_REPORT, SESSION_ERROR
};
int session_manager_init(session_manager *manager);
void session_manager_destroy(session_manager *manager);
/* On failure caller retains fd. Each accepted session has one worker owner. */
session *session_add(session_manager *manager, int fd, const char *ip);
/* Main thread only, for failures before worker creation. */
void session_remove(session_manager *manager, session *client);
/* Last worker action: closes fd, invalidates session, wakes shutdown waiter. */
void session_finish(session_manager *manager, session *client);
/* Main thread only: join and free completed workers outside the mutex. */
void session_reap(session_manager *manager);
/* Stops additions, interrupts socket I/O, waits for owners, joins all workers. */
void session_shutdown(session_manager *manager);
bool session_authenticated(session_manager *manager, const session *client);
void session_login(session_manager *manager, session *client, const char *username);
bool session_logout(session_manager *manager, session *client);
enum session_result session_hello(session_manager *manager, session *client,
                                  uint32_t id, uint16_t port);

/* Canonical filename tokens are validated by the dispatcher. File ownership
 * and index changes are protected by the session mutex, together with HELLO,
 * logout and disconnect. No socket/log I/O occurs under that mutex.
 */
enum session_result session_share(session_manager *manager, session *client,
                                  const char *filename, uint64_t size);
enum session_result session_unshare(session_manager *manager, session *client,
                                    const char *filename);
/* Caller frees the snapshot. Zero results sets matches=NULL and count=0.
 * Snapshot records remain valid after their owning sessions disconnect.
 */
enum session_result session_search(session_manager *manager, session *client,
                                   const char *filename, file_match **matches, size_t *count);
enum session_result session_report(session_manager *manager, session *client,
                                   uint32_t target_id, const char *filename);

#endif
