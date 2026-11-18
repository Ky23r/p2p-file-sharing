#include "session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct shared_file {
    char filename[PROTOCOL_FILENAME_TOKEN_MAX + 1];
    uint64_t size;
    shared_file *next;
};

/* All file-index helpers run with manager->mutex held. Keeping advertisements
 * on their owning session makes identity changes and cleanup atomic with search.
 */
static void clear_files(session *client)
{
    while (client->files != NULL) {
        shared_file *file = client->files;
        client->files = file->next;
        free(file);
    }
}

static shared_file *find_file(const session *client, const char *filename)
{
    for (shared_file *file = client->files; file != NULL; file = file->next)
        if (strcmp(file->filename, filename) == 0)
            return file;
    return NULL;
}

static enum session_result require_peer(const session *client)
{
    if (!client->authenticated)
        return SESSION_UNAUTHORIZED;
    return client->peer_registered ? SESSION_OK : SESSION_HELLO_REQUIRED;
}

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
    if (manager->stopping) {
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
    clear_files(client);
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
    clear_files(client);
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

void session_login(session_manager *manager, session *client)
{
    pthread_mutex_lock(&manager->mutex);
    client->authenticated = true;
    pthread_mutex_unlock(&manager->mutex);
}

bool session_logout(session_manager *manager, session *client)
{
    pthread_mutex_lock(&manager->mutex);
    bool was_authenticated = client->authenticated;
    clear_files(client);
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
            if (client->peer_registered && client->client_id != id)
                clear_files(client);
            client->client_id = id;
            client->port = port;
            client->peer_registered = true;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

enum session_result session_share(session_manager *manager, session *client,
                                  const char *filename, uint64_t size)
{
    pthread_mutex_lock(&manager->mutex);
    enum session_result result = require_peer(client);
    if (result == SESSION_OK) {
        shared_file *file = find_file(client, filename);
        if (file != NULL) {
            file->size = size;
        } else {
            file = malloc(sizeof(*file));
            if (file == NULL) {
                result = SESSION_ERROR;
            } else {
                strcpy(file->filename, filename);
                file->size = size;
                file->next = client->files;
                client->files = file;
            }
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

enum session_result session_unshare(session_manager *manager, session *client,
                                    const char *filename)
{
    pthread_mutex_lock(&manager->mutex);
    enum session_result result = require_peer(client);
    if (result == SESSION_OK) {
        shared_file **link = &client->files;
        while (*link != NULL && strcmp((*link)->filename, filename) != 0)
            link = &(*link)->next;
        if (*link == NULL) {
            result = SESSION_FILE_NOT_FOUND;
        } else {
            shared_file *file = *link;
            *link = file->next;
            free(file);
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    return result;
}

static int compare_matches(const void *left, const void *right)
{
    const file_match *a = left, *b = right;
    return (a->client_id > b->client_id) - (a->client_id < b->client_id);
}

enum session_result session_search(session_manager *manager, session *client,
                                   const char *filename, file_match **matches, size_t *count)
{
    *matches = NULL;
    *count = 0;
    pthread_mutex_lock(&manager->mutex);
    enum session_result result = require_peer(client);
    if (result == SESSION_OK) {
        size_t found = 0;
        for (session *peer = manager->head; peer != NULL; peer = peer->next)
            if (peer->peer_registered && find_file(peer, filename) != NULL)
                found++;
        file_match *snapshot = found == 0 ? NULL : calloc(found, sizeof(*snapshot));
        if (found != 0 && snapshot == NULL) {
            result = SESSION_ERROR;
        } else {
            size_t i = 0;
            for (session *peer = manager->head; peer != NULL; peer = peer->next) {
                if (!peer->peer_registered)
                    continue;
                shared_file *file = find_file(peer, filename);
                if (file == NULL)
                    continue;
                snapshot[i].size = file->size;
                snapshot[i].client_id = peer->client_id;
                strcpy(snapshot[i].ip, peer->ip);
                snapshot[i].port = peer->port;
                i++;
            }
            *matches = snapshot;
            *count = found;
        }
    }
    pthread_mutex_unlock(&manager->mutex);
    if (*count > 1)
        qsort(*matches, *count, sizeof(**matches), compare_matches);
    return result;
}
