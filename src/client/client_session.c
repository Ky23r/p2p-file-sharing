#include "client_session.h"
#include "net_io.h"
#include "protocol.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_KEEPALIVE_SECONDS 10

struct client_session {
    net_reader server;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t heartbeat;
    int failure[2];
    atomic_bool stopping;
    bool failed;
    bool announced;
    uint32_t id;
    uint16_t port;
    shared_index *index;
    const char *downloads;
    ClientSignals *signals;
    size_t result_count;
    file_result results[FILE_ENTRY_LIMIT];
};

enum reply_output { REPLY_BACKGROUND, REPLY_ALL, REPLY_ERRORS };

/* Every function ending in _locked requires the control exchange mutex. */
static void deactivate_locked(client_session *client)
{
    client->announced = false;
    client->result_count = 0;
    shared_index_set_online(client->index, false);
}

static enum request_result fail_locked(client_session *client)
{
    if (!client->failed) {
        char notification = 1;
        ssize_t written;
        do {
            written = write(client->failure[1], &notification, 1);
        } while (written < 0 && errno == EINTR);
    }
    client->failed = true;
    deactivate_locked(client);
    shutdown(client->server.fd, SHUT_RDWR);
    if (!client->stopping && client_signals_number(client->signals) == 0)
        fputs("Invalid response from server or control connection failed. Restart the client to reconnect.\n", stderr);
    return REQUEST_FAILED;
}

static int read_response_locked(client_session *client, char *line, size_t *length)
{
    int status = recv_line(&client->server, line, PROTOCOL_MAX_LINE + 1, length);
    if (status != NET_LINE_OK)
        fail_locked(client);
    return status;
}

static enum request_result error_response_locked(client_session *client, const char *line,
                                                  size_t length, enum reply_output output)
{
    if (!file_error(line, length))
        return fail_locked(client);
    if (output == REPLY_BACKGROUND)
        fprintf(stderr, "Peer keepalive rejected: %s.\n", line);
    else
        puts(line);
    if (strcmp(line, "ERR SERVER_BUSY") == 0 || strcmp(line, "ERR LINE_TOO_LONG") == 0)
        return fail_locked(client);
    return REQUEST_REJECTED;
}

static enum request_result exchange_locked(client_session *client, const char *request,
                                           const char *expected, enum reply_output output)
{
    if (client->failed)
        return REQUEST_FAILED;
    if (send_all(client->server.fd, request, strlen(request)) < 0)
        return fail_locked(client);
    char response[PROTOCOL_MAX_LINE + 1];
    size_t length;
    if (read_response_locked(client, response, &length) != NET_LINE_OK)
        return REQUEST_FAILED;
    if (length == strlen(expected) && memcmp(response, expected, length) == 0) {
        if (output == REPLY_ALL)
            puts(response);
        return REQUEST_OK;
    }
    return error_response_locked(client, response, length, output);
}

static enum request_result hello_locked(client_session *client, enum reply_output output)
{
    char request[64];
    snprintf(request, sizeof(request), "HELLO %" PRIu32 " %u\n", client->id, (unsigned)client->port);
    return exchange_locked(client, request, "OK HELLO", output);
}

static enum request_result withdraw_locked(client_session *client, const shared_file *entry)
{
    if (!entry->announced)
        return REQUEST_OK;
    char encoded[FILE_NAME_HEX_CAP], request[FILE_NAME_HEX_CAP + 16];
    file_hex_encode(entry->name, encoded, sizeof(encoded));
    snprintf(request, sizeof(request), "UNSHARE %s\n", encoded);
    return exchange_locked(client, request, "OK UNSHARED", REPLY_ALL);
}

static enum request_result publish_locked(client_session *client, const shared_file *entry)
{
    shared_index_mark(client->index, entry->name, false, entry->size);
    uint64_t size;
    int source = shared_file_open(entry->path, &size);
    if (source < 0) {
        fprintf(stderr, "Share %s remains pending: %s.\n", entry->name, strerror(errno));
        if (withdraw_locked(client, entry) == REQUEST_FAILED)
            return REQUEST_FAILED;
        return REQUEST_REJECTED;
    }
    close(source);
    char name[FILE_NAME_HEX_CAP], request[FILE_NAME_HEX_CAP + 64];
    file_hex_encode(entry->name, name, sizeof(name));
    snprintf(request, sizeof(request), "SHARE %s %" PRIu64 "\n", name, size);
    enum request_result result = exchange_locked(client, request, "OK SHARED", REPLY_ERRORS);
    if (result == REQUEST_OK) {
        shared_index_mark(client->index, entry->name, true, size);
        puts("OK SHARED");
    } else if (result != REQUEST_FAILED && withdraw_locked(client, entry) == REQUEST_FAILED)
        return REQUEST_FAILED;
    return result;
}

static void *keepalive_loop(void *argument)
{
    client_session *client = argument;
    pthread_mutex_lock(&client->mutex);
    while (!client->stopping && !client->failed) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            fail_locked(client);
            break;
        }
        deadline.tv_sec += CLIENT_KEEPALIVE_SECONDS;
        int status = 0;
        while (status == 0 && !client->stopping && !client->failed)
            status = pthread_cond_timedwait(&client->wake, &client->mutex, &deadline);
        if (client->stopping || client->failed)
            break;
        if (status != ETIMEDOUT) {
            fail_locked(client);
            break;
        }
        /* A rejected HELLO leaves the server's previous registration intact.
         * Transport/framing failures already deactivate the peer in fail_locked.
         */
        if (client->announced)
            (void)hello_locked(client, REPLY_BACKGROUND);
    }
    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

client_session *client_session_start(int fd, uint32_t id, uint16_t port,
                                     shared_index *index, const char *downloads, ClientSignals *signals)
{
    client_session *client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    atomic_init(&client->stopping, false);
    client->failure[0] = client->failure[1] = -1;
    net_reader_init(&client->server, fd);
    client->id = id;
    client->port = port;
    client->index = index;
    client->downloads = downloads;
    client->signals = signals;
    int error = pthread_mutex_init(&client->mutex, NULL);
    if (error != 0)
        goto free_client;
    error = pthread_cond_init(&client->wake, NULL);
    if (error != 0)
        goto destroy_mutex;
    if (pipe(client->failure) != 0 ||
        fcntl(client->failure[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(client->failure[1], F_SETFD, FD_CLOEXEC) < 0) {
        error = errno;
        goto close_failure;
    }
    sigset_t previous_mask;
    if (client_signals_block_workers(&previous_mask) != 0) {
        error = errno;
        goto close_failure;
    }
    error = pthread_create(&client->heartbeat, NULL, keepalive_loop, client);
    int mask_error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error == 0 && mask_error != 0) {
        client_session_stop(client);
        errno = mask_error;
        return NULL;
    }
    if (error == 0)
        return client;
close_failure:
    if (client->failure[0] >= 0)
        close(client->failure[0]);
    if (client->failure[1] >= 0)
        close(client->failure[1]);
    pthread_cond_destroy(&client->wake);
destroy_mutex:
    pthread_mutex_destroy(&client->mutex);
free_client:
    free(client);
    errno = error;
    return NULL;
}

void client_session_stop(client_session *client)
{
    if (client == NULL)
        return;
    /* Interrupt a keepalive already blocked in socket I/O before taking its lock. */
    client->stopping = true;
    shutdown(client->server.fd, SHUT_RDWR);
    pthread_mutex_lock(&client->mutex);
    deactivate_locked(client);
    pthread_cond_signal(&client->wake);
    pthread_mutex_unlock(&client->mutex);
    pthread_join(client->heartbeat, NULL);
    close(client->failure[0]);
    close(client->failure[1]);
    pthread_cond_destroy(&client->wake);
    pthread_mutex_destroy(&client->mutex);
    free(client);
}

bool client_session_failed(client_session *client)
{
    pthread_mutex_lock(&client->mutex);
    bool failed = client->failed;
    pthread_mutex_unlock(&client->mutex);
    return failed;
}

int client_session_failure_fd(client_session *client)
{
    return client->failure[0];
}

enum request_result client_exchange(client_session *client, const char *request, const char *expected)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = exchange_locked(client, request, expected, REPLY_ALL);
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_hello(client_session *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = hello_locked(client, REPLY_ERRORS);
    if (result == REQUEST_OK) {
        client->announced = true;
        shared_index_set_online(client->index, true);
        puts("OK HELLO");
        shared_file entry;
        for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
            if (publish_locked(client, &entry) == REQUEST_FAILED) {
                result = REQUEST_FAILED;
                break;
            }
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_logout(client_session *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = exchange_locked(client, "LOGOUT\n", "OK LOGGED_OUT", REPLY_ERRORS);
    if (result == REQUEST_OK) {
        deactivate_locked(client);
        puts("OK LOGGED_OUT");
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_share(client_session *client, const char *path)
{
    char name[FILE_NAME_CAP];
    if (path == NULL || shared_index_add(client->index, path, name) != 0) {
        fprintf(stderr, "Cannot share file: %s.\n", path == NULL ? "missing path" : strerror(errno));
        return REQUEST_REJECTED;
    }
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_OK;
    if (!client->announced) {
        printf("OK SAVED %s\n", name);
    } else {
        shared_file entry;
        for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
            if (strcmp(entry.name, name) == 0) {
                result = publish_locked(client, &entry);
                break;
            }
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_unshare(client_session *client, const char *name)
{
    if (name == NULL || !file_name_valid(name)) {
        fputs("Invalid shared filename.\n", stderr);
        return REQUEST_REJECTED;
    }
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_OK;
    if (client->announced) {
        char encoded[FILE_NAME_HEX_CAP], request[FILE_NAME_HEX_CAP + 16];
        file_hex_encode(name, encoded, sizeof(encoded));
        snprintf(request, sizeof(request), "UNSHARE %s\n", encoded);
        result = exchange_locked(client, request, "OK UNSHARED", REPLY_ERRORS);
    }
    if (result == REQUEST_OK) {
        if (shared_index_remove(client->index, name) != 0) {
            fprintf(stderr, "Cannot save share removal: %s. Retry unshare before hello/login.\n", strerror(errno));
            puts("ERR LOCAL_STORAGE");
            result = REQUEST_REJECTED;
        } else {
            puts("OK UNSHARED");
        }
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

void client_shares(client_session *client)
{
    shared_file entry;
    for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
        printf("SHARE %s %" PRIu64 " %s\n", entry.name, entry.size, entry.announced ? "ANNOUNCED" : "PENDING");
}

static bool parse_result(char *line, size_t length, file_result *result)
{
    char *tokens[6];
    uint64_t id, port, size;
    struct in_addr address;
    if (!file_tokens(line, length, tokens, 6) || strcmp(tokens[0], "FILE") != 0 ||
        !file_decimal(tokens[1], UINT32_MAX, &id) ||
        strlen(tokens[2]) >= sizeof(result->ip) || inet_pton(AF_INET, tokens[2], &address) != 1 ||
        !file_decimal(tokens[3], UINT16_MAX, &port) || port == 0 ||
        !file_decimal(tokens[4], FILE_SIZE_MAX, &size) || !file_name_decode(tokens[5], result->name))
        return false;
    result->client_id = (uint32_t)id;
    result->port = (uint16_t)port;
    result->size = size;
    strcpy(result->ip, tokens[2]);
    return true;
}

enum request_result client_search(client_session *client, const char *query)
{
    pthread_mutex_lock(&client->mutex);
    client->result_count = 0;
    enum request_result result = REQUEST_REJECTED;
    char encoded[FILE_NAME_HEX_CAP];
    if (query == NULL || !file_name_valid(query)) {
        fputs("Invalid search query.\n", stderr);
        goto unlock;
    }
    if (!client->announced) {
        fputs("Log in and announce the peer before searching.\n", stderr);
        goto unlock;
    }
    file_hex_encode(query, encoded, sizeof(encoded));
    char line[PROTOCOL_MAX_LINE + 1], request[FILE_NAME_HEX_CAP + 16];
    snprintf(request, sizeof(request), "SEARCH %s\n", encoded);
    if (client->failed || send_all(client->server.fd, request, strlen(request)) < 0) {
        result = fail_locked(client);
        goto unlock;
    }
    size_t length;
    if (read_response_locked(client, line, &length) != NET_LINE_OK) {
        result = REQUEST_FAILED;
        goto unlock;
    }
    if (file_error(line, length)) {
        result = error_response_locked(client, line, length, REPLY_ALL);
        goto unlock;
    }
    char *tokens[3];
    uint64_t count;
    if (!file_tokens(line, length, tokens, 3) || strcmp(tokens[0], "OK") != 0 ||
        strcmp(tokens[1], "RESULTS") != 0 || !file_decimal(tokens[2], FILE_ENTRY_LIMIT, &count)) {
        result = fail_locked(client);
        goto unlock;
    }
    for (size_t i = 0; i < (size_t)count; ++i) {
        file_result *entry = &client->results[i];
        if (read_response_locked(client, line, &length) != NET_LINE_OK) {
            result = REQUEST_FAILED;
            goto unlock;
        }
        if (!parse_result(line, length, entry) || strstr(entry->name, query) == NULL) {
            result = fail_locked(client);
            goto unlock;
        }
        if (i != 0) {
            file_result *previous = &client->results[i - 1];
            int order = strcmp(previous->name, entry->name);
            if (order > 0 || (order == 0 && previous->client_id >= entry->client_id)) {
                result = fail_locked(client);
                goto unlock;
            }
        }
    }
    client->result_count = (size_t)count;
    printf("OK RESULTS %zu\n", client->result_count);
    for (size_t i = 0; i < client->result_count; ++i) {
        file_result *entry = &client->results[i];
        printf("%zu: %s (client %" PRIu32 ", %s:%u, %" PRIu64 " bytes)\n",
               i + 1, entry->name, entry->client_id, entry->ip, (unsigned)entry->port, entry->size);
    }
    result = REQUEST_OK;
unlock:
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_download(client_session *client, const char *number)
{
    pthread_mutex_lock(&client->mutex);
    uint64_t selection;
    if (!client->announced || number == NULL ||
        !file_decimal(number, client->result_count, &selection) || selection == 0) {
        pthread_mutex_unlock(&client->mutex);
        fputs("Choose a valid result number from the latest search.\n", stderr);
        return REQUEST_REJECTED;
    }
    file_result entry = client->results[selection - 1];
    pthread_mutex_unlock(&client->mutex);
    const char *failure = download_file(&entry, client->downloads, client->signals);
    if (failure == NULL) {
        printf("OK DOWNLOADED %s\n", entry.name);
        return REQUEST_OK;
    }
    if (strcmp(failure, "CANCELLED") == 0)
        return REQUEST_FAILED;
    fprintf(stderr, "Download %s from client %" PRIu32 " at %s:%u failed: %s (%s).\n",
            entry.name, entry.client_id, entry.ip, (unsigned)entry.port, failure, strerror(errno));
    if (strcmp(failure, "LOCAL_ERROR") == 0)
        return REQUEST_REJECTED;
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_REJECTED;
    if (!client->failed && client->announced) {
        char encoded[FILE_NAME_HEX_CAP], request[FILE_NAME_HEX_CAP + 100];
        file_hex_encode(entry.name, encoded, sizeof(encoded));
        snprintf(request, sizeof(request), "REPORT_FAIL %" PRIu32 " %s %s\n", entry.client_id, encoded, failure);
        if (exchange_locked(client, request, "OK REPORTED", REPLY_ALL) == REQUEST_FAILED)
            result = REQUEST_FAILED;
    } else {
        fputs("Cannot report the transfer failure: control session is unavailable.\n", stderr);
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}
