#ifndef P2P_SESSION_H
#define P2P_SESSION_H

#include "protocol.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SESSION_LIMIT 128U

typedef struct session {
    int fd;
    char username[USERNAME_MAX + 1];
    bool authenticated;
    uint32_t client_id;
    uint16_t port;
    char ip[INET_ADDRSTRLEN];
    bool peer_registered;
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

enum session_result { SESSION_OK, SESSION_UNAUTHORIZED, SESSION_ID_IN_USE };
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

#endif
