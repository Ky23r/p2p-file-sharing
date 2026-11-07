#include "session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int session_manager_init(session_manager *manager)
{
    memset(manager, 0, sizeof(*manager));
    int error = pthread_mutex_init(&manager->mutex, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    error = pthread_cond_init(&manager->empty, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&manager->mutex);
        errno = error;
        return -1;
    }
    return 0;
}

void session_manager_destroy(session_manager *manager)
{
    pthread_cond_destroy(&manager->empty);
    pthread_mutex_destroy(&manager->mutex);
}

session *session_add(session_manager *manager, int fd, const char *ip)
{
    session *client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    client->fd = fd;
    strcpy(client->ip, ip);
    pthread_mutex_lock(&manager->mutex);
    if (manager->stopping || manager->count >= SESSION_LIMIT) {
        pthread_mutex_unlock(&manager->mutex);
        free(client);
        errno = EBUSY;
        return NULL;
    }
    client->next = manager->head;
    manager->head = client;
    manager->count++;
    pthread_mutex_unlock(&manager->mutex);
    return client;
}

void session_remove(session_manager *manager, session *client)
{
    pthread_mutex_lock(&manager->mutex);
    session **link = &manager->head;
    while (*link != client)
        link = &(*link)->next;
    *link = client->next;
    /* Close while locked so shutdown cannot act on a recycled descriptor. */
    close(client->fd);
    memset(client, 0, sizeof(*client));
    free(client);
    manager->count--;
    if (manager->count == 0)
        pthread_cond_signal(&manager->empty);
    pthread_mutex_unlock(&manager->mutex);
}

void session_shutdown(session_manager *manager)
{
    pthread_mutex_lock(&manager->mutex);
    manager->stopping = true;
    for (session *client = manager->head; client != NULL; client = client->next)
        if (!client->finished)
            shutdown(client->fd, SHUT_RDWR);
    while (manager->count != 0)
        pthread_cond_wait(&manager->empty, &manager->mutex);
    pthread_mutex_unlock(&manager->mutex);
    session_reap(manager);
}

void session_finish(session_manager *manager, session *client)
{
    pthread_mutex_lock(&manager->mutex);
    close(client->fd);
    client->fd = -1;
    memset(client->username, 0, sizeof(client->username));
    client->authenticated = false;
    client->peer_registered = false;
    client->finished = true;
    manager->count--;
    if (manager->count == 0)
        pthread_cond_signal(&manager->empty);
    pthread_mutex_unlock(&manager->mutex);
}

void session_reap(session_manager *manager)
{
    for (;;) {
        pthread_mutex_lock(&manager->mutex);
        session **link = &manager->head;
        while (*link != NULL && !(*link)->finished)
            link = &(*link)->next;
        session *client = *link;
        if (client != NULL)
            *link = client->next;
        pthread_mutex_unlock(&manager->mutex);
        if (client == NULL)
            break;
        /* Joining outside the lock also waits for OpenSSL thread cleanup. */
        pthread_join(client->thread, NULL);
        memset(client, 0, sizeof(*client));
        free(client);
    }
}

bool session_authenticated(session_manager *manager, const session *client)
{
    pthread_mutex_lock(&manager->mutex);
    bool authenticated = client->authenticated;
    pthread_mutex_unlock(&manager->mutex);
    return authenticated;
}

void session_login(session_manager *manager, session *client, const char *username)
{
    pthread_mutex_lock(&manager->mutex);
    strcpy(client->username, username);
    client->authenticated = true;
    pthread_mutex_unlock(&manager->mutex);
}

bool session_logout(session_manager *manager, session *client)
{
    pthread_mutex_lock(&manager->mutex);
    bool was_authenticated = client->authenticated;
    memset(client->username, 0, sizeof(client->username));
    client->authenticated = false;
    client->peer_registered = false;
    client->client_id = 0;
    client->port = 0;
    pthread_mutex_unlock(&manager->mutex);
    return was_authenticated;
}

enum session_result session_hello(session_manager *manager, session *client,
                                  uint32_t id, uint16_t port)
{
    enum session_result result = SESSION_OK;
    pthread_mutex_lock(&manager->mutex);
    if (!client->authenticated) {
        result = SESSION_UNAUTHORIZED;
    } else {
        for (session *other = manager->head; other != NULL; other = other->next) {
            if (other != client && other->peer_registered && other->client_id == id) {
                result = SESSION_ID_IN_USE;
                break;
            }
        }
        if (result == SESSION_OK) {
            client->client_id = id;
            client->port = port;
            client->peer_registered = true;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}
