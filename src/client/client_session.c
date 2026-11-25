#include "client_session.h"
#include "net_io.h"
#include "protocol.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_KEEPALIVE_SECONDS 10

typedef struct PendingUnshare {
    struct PendingUnshare *next;
    char token[FILE_NAME_TOKEN_CAP];
} PendingUnshare;

struct ClientSession {
    net_reader server;
    char *response;
    size_t response_capacity;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t heartbeat;
    int failure[2];
    atomic_bool stopping;
    atomic_bool failed;
    bool hello_staged;
    bool announced;
    uint32_t id;
    uint16_t port;
    SharedIndex *index;
    PendingUnshare *withdrawals;
    const char *downloads;
    ClientSignals *signals;
    size_t result_count;
    size_t result_capacity;
    FileSearchResult *results;
};

enum reply_output { REPLY_KEEPALIVE, REPLY_UNSHARE_RETRY, REPLY_ALL, REPLY_ERRORS };

/* Every function ending in _locked requires the control exchange mutex. */
static void remove_withdrawal_locked(PendingUnshare **link)
{
    PendingUnshare *withdrawal = *link;
    *link = withdrawal->next;
    free(withdrawal);
}

static PendingUnshare **find_withdrawal_locked(ClientSession *client, const char *token)
{
    PendingUnshare **link = &client->withdrawals;
    while (*link != NULL && strcmp((*link)->token, token) != 0)
        link = &(*link)->next;
    return link;
}

static void deactivate_locked(ClientSession *client)
{
    client->hello_staged = false;
    client->announced = false;
    client->result_count = 0;
    while (client->withdrawals != NULL)
        remove_withdrawal_locked(&client->withdrawals);
    shared_index_set_online(client->index, false);
}

static void close_failed_session_locked(ClientSession *client)
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
}

static enum request_result fail_locked(ClientSession *client)
{
    close_failed_session_locked(client);
    if (!client->stopping && client_signals_number(client->signals) == 0)
        fputs("Invalid response from server or control connection failed. Restart the client to reconnect.\n", stderr);
    return REQUEST_FAILED;
}

static int read_response_locked(ClientSession *client, size_t *length)
{
    int status = recv_line(&client->server, &client->response, &client->response_capacity, length);
    if (status != NET_LINE_OK)
        fail_locked(client);
    return status;
}

static enum request_result error_response_locked(ClientSession *client, const char *line,
                                                  size_t length, enum reply_output output)
{
    if (!protocol_error(line, length))
        return fail_locked(client);
    if (output == REPLY_KEEPALIVE)
        fprintf(stderr, "Peer keepalive rejected: %s.\n", line);
    else if (output == REPLY_UNSHARE_RETRY)
        fprintf(stderr, "Unshare retry rejected: %s.\n", line);
    else
        puts(line);
    if (memcmp(line, STATUS_SERVICE_UNAVAILABLE, 3) == 0) {
        close_failed_session_locked(client);
        return REQUEST_FAILED;
    }
    return REQUEST_REJECTED;
}

static bool request_locked(ClientSession *client, const char *request, size_t *length)
{
    if (client->failed)
        return false;
    if (send_all(client->server.fd, request, strlen(request)) < 0) {
        fail_locked(client);
        return false;
    }
    return read_response_locked(client, length) == NET_LINE_OK;
}

static bool response_is(const char *response, size_t length, const char *expected)
{
    return length == strlen(expected) && memcmp(response, expected, length) == 0;
}

static enum request_result exchange_locked(ClientSession *client, const char *request,
                                           enum reply_output output)
{
    size_t length;
    if (!request_locked(client, request, &length))
        return REQUEST_FAILED;
    if (protocol_reply_is(client->response, length, STATUS_OK)) {
        if (output == REPLY_ALL)
            puts(client->response);
        return REQUEST_OK;
    }
    return error_response_locked(client, client->response, length, output);
}

static enum request_result hello_locked(ClientSession *client, enum reply_output output)
{
    char request[64];
    snprintf(request, sizeof(request), CMD_HELLO " %" PRIu32 " %u" CRLF, client->id, (unsigned)client->port);
    enum request_result result = exchange_locked(client, request, output);
    if (result == REQUEST_OK)
        client->hello_staged = true;
    return result;
}

static enum request_result unshare_locked(ClientSession *client, const char *token, enum reply_output output)
{
    char request[FILE_NAME_TOKEN_CAP + sizeof(CMD_UNSHARE " " CRLF)];
    snprintf(request, sizeof(request), CMD_UNSHARE " %s" CRLF, token);
    size_t length;
    if (!request_locked(client, request, &length))
        return REQUEST_FAILED;
    /* An absent advertisement also completes the local unshare. */
    if (protocol_reply_is(client->response, length, STATUS_OK) ||
        response_is(client->response, length, REPLY_FILE_NOT_SHARED))
        return REQUEST_OK;
    return error_response_locked(client, client->response, length, output);
}

static enum request_result withdraw_locked(ClientSession *client, PendingUnshare **link, enum reply_output output)
{
    enum request_result result = unshare_locked(client, (*link)->token, output);
    if (result == REQUEST_OK)
        remove_withdrawal_locked(link);
    return result;
}

static enum request_result retry_withdrawals_locked(ClientSession *client)
{
    PendingUnshare **link = &client->withdrawals;
    while (*link != NULL) {
        enum request_result result = withdraw_locked(client, link, REPLY_UNSHARE_RETRY);
        if (result == REQUEST_FAILED)
            return result;
        if (result == REQUEST_REJECTED)
            link = &(*link)->next;
    }
    return REQUEST_OK;
}

static enum request_result publish_locked(ClientSession *client, const SharedFile *entry)
{
    shared_index_mark(client->index, entry->name, false, 0);
    char token[FILE_NAME_TOKEN_CAP];
    if (!protocol_encode_filename(entry->name, token, sizeof(token))) {
        fprintf(stderr, "Share %s remains pending: invalid filename.\n", entry->name);
        return REQUEST_REJECTED;
    }
    uint64_t size;
    int source = shared_file_open(entry->path, &size);
    enum request_result result = REQUEST_REJECTED;
    if (source < 0) {
        fprintf(stderr, "Share %s remains pending: %s.\n", entry->name, strerror(errno));
    } else {
        close(source);
        char request[FILE_NAME_TOKEN_CAP + 64];
        snprintf(request, sizeof(request), CMD_SHARE " %" PRIu32 " %s %" PRIu64 CRLF,
                 client->id, token, size);
        result = exchange_locked(client, request, REPLY_ERRORS);
        if (result == REQUEST_OK) {
            /* The server has replaced any listing from before this SHARE. */
            PendingUnshare **withdrawal = find_withdrawal_locked(client, token);
            if (*withdrawal != NULL)
                remove_withdrawal_locked(withdrawal);
            shared_index_mark(client->index, entry->name, true, size);
            puts(STATUS_OK " SHARED");
            return REQUEST_OK;
        }
    }
    if (result != REQUEST_FAILED && entry->announced) {
        enum request_result removal = unshare_locked(client, token, REPLY_ERRORS);
        if (removal == REQUEST_FAILED)
            return removal;
        if (removal == REQUEST_OK)
            puts(STATUS_OK " UNSHARED");
    }
    return result;
}

static enum request_result republish_locked(ClientSession *client)
{
    SharedFile entry;
    for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
        if (publish_locked(client, &entry) == REQUEST_FAILED)
            return REQUEST_FAILED;
    return REQUEST_OK;
}

static void *keepalive_loop(void *argument)
{
    ClientSession *client = argument;
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
        /* A rejected HELLO leaves the server's previous announcement intact.
         * Transport/framing failures already deactivate the peer in fail_locked.
         */
        if (hello_locked(client, REPLY_KEEPALIVE) != REQUEST_FAILED && client->announced)
            (void)retry_withdrawals_locked(client);
    }
    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

ClientSession *client_session_start(int fd, uint32_t id, uint16_t port,
                                     SharedIndex *index, const char *downloads, ClientSignals *signals)
{
    ClientSession *client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    atomic_init(&client->stopping, false);
    atomic_init(&client->failed, false);
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

void client_session_interrupt(ClientSession *client)
{
    if (client != NULL && !atomic_exchange(&client->stopping, true))
        shutdown(client->server.fd, SHUT_RDWR);
}

void client_session_stop(ClientSession *client)
{
    if (client == NULL)
        return;
    client_session_interrupt(client);
    pthread_mutex_lock(&client->mutex);
    deactivate_locked(client);
    pthread_cond_signal(&client->wake);
    pthread_mutex_unlock(&client->mutex);
    pthread_join(client->heartbeat, NULL);
    close(client->failure[0]);
    close(client->failure[1]);
    pthread_cond_destroy(&client->wake);
    pthread_mutex_destroy(&client->mutex);
    free(client->results);
    free(client->response);
    free(client);
}

bool client_session_failed(ClientSession *client)
{
    return client->failed;
}

int client_session_failure_fd(ClientSession *client)
{
    return client->failure[0];
}

enum request_result client_stage_hello(ClientSession *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = hello_locked(client, REPLY_ERRORS);
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_account(ClientSession *client, char *request, size_t capacity,
                                   bool registering)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_OK;
    if (!registering && !client->hello_staged)
        result = hello_locked(client, REPLY_ERRORS);
    if (result == REQUEST_OK)
        result = exchange_locked(client, request, REPLY_ALL);
    OPENSSL_cleanse(request, capacity);
    if (!registering && result == REQUEST_OK) {
        client->announced = true;
        shared_index_set_online(client->index, true);
        puts(STATUS_OK " " CMD_HELLO);
        result = republish_locked(client);
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_hello(ClientSession *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = hello_locked(client, REPLY_ERRORS);
    if (result == REQUEST_OK) {
        puts(STATUS_OK " " CMD_HELLO);
        if (client->announced && (result = retry_withdrawals_locked(client)) == REQUEST_OK)
            result = republish_locked(client);
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_logout(ClientSession *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = exchange_locked(client, CMD_LOGOUT CRLF, REPLY_ERRORS);
    if (result == REQUEST_OK) {
        deactivate_locked(client);
        puts(STATUS_OK " LOGGED_OUT");
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_share(ClientSession *client, const char *path)
{
    char name[FILE_NAME_CAP];
    if (path == NULL || shared_index_add(client->index, path, name) != 0) {
        fprintf(stderr, "Cannot share file: %s.\n", path == NULL ? "missing path" : strerror(errno));
        return REQUEST_REJECTED;
    }
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_OK;
    if (!client->announced) {
        printf("Saved %s\n", name);
    } else {
        SharedFile entry;
        for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
            if (strcmp(entry.name, name) == 0) {
                result = publish_locked(client, &entry);
                break;
            }
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_unshare(ClientSession *client, const char *name)
{
    if (!protocol_valid_filename(name)) {
        fputs("Invalid shared filename.\n", stderr);
        return REQUEST_REJECTED;
    }
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_REJECTED;
    char token[FILE_NAME_TOKEN_CAP];
    PendingUnshare **withdrawal = NULL;
    PendingUnshare *reserved = NULL;
    if (client->announced && protocol_encode_filename(name, token, sizeof(token))) {
        withdrawal = find_withdrawal_locked(client, token);
        if (*withdrawal == NULL) {
            reserved = malloc(sizeof(*reserved));
            if (reserved == NULL) {
                fprintf(stderr, "Cannot prepare share removal: %s.\n", strerror(errno));
                goto unlock;
            }
            reserved->next = NULL;
            strcpy(reserved->token, token);
        }
    }
    if (shared_index_remove(client->index, name) != 0) {
        fprintf(stderr, "Cannot save share removal: %s.\n", strerror(errno));
        free(reserved);
        goto unlock;
    }
    result = REQUEST_OK;
    if (withdrawal != NULL) {
        if (reserved != NULL)
            *withdrawal = reserved;
        result = withdraw_locked(client, withdrawal, REPLY_ERRORS);
        if (result == REQUEST_REJECTED)
            fprintf(stderr, "Removed %s locally. Server withdrawal will be retried.\n", name);
    }
    if (result == REQUEST_OK)
        puts(STATUS_OK " UNSHARED");
unlock:
    pthread_mutex_unlock(&client->mutex);
    return result;
}

void client_withdraw_stale_share(void *context, const char *name)
{
    ClientSession *client = context;
    pthread_mutex_lock(&client->mutex);
    if (client->announced && !client->stopping) {
        /* The source may have been republished since the failed peer request. */
        int source = shared_index_open(client->index, name);
        if (source >= 0) {
            close(source);
        } else if (shared_file_stale(errno)) {
            char token[FILE_NAME_TOKEN_CAP];
            if (protocol_encode_filename(name, token, sizeof(token)) &&
                unshare_locked(client, token, REPLY_ERRORS) == REQUEST_OK)
                shared_index_mark(client->index, name, false, 0);
        }
    }
    pthread_mutex_unlock(&client->mutex);
}

void client_shares(ClientSession *client)
{
    SharedFile entry;
    for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
        printf(CMD_SHARE " %s %" PRIu64 " %s\n", entry.name, entry.size,
               entry.announced ? "ANNOUNCED" : "PENDING");
}

static bool parse_result(char *line, size_t length, FileSearchResult *result)
{
    const size_t prefix = sizeof(REPLY_CONTINUATION) - 1;
    char *tokens[5];
    uint64_t id, port, advertised_size;
    struct in_addr address;
    if (length <= prefix || memcmp(line, REPLY_CONTINUATION, prefix) != 0 ||
        protocol_tokens(line + prefix, length - prefix, tokens, 5) != 5 ||
        !protocol_decode_filename(tokens[0], result->name, sizeof(result->name)) ||
        !protocol_parse_uint64(tokens[1], UINT64_MAX, &advertised_size) ||
        !protocol_parse_uint64(tokens[2], UINT32_MAX, &id) ||
        strlen(tokens[3]) >= sizeof(result->ip) || inet_pton(AF_INET, tokens[3], &address) != 1 ||
        !protocol_parse_uint64(tokens[4], UINT16_MAX, &port) || port == 0)
        return false;
    result->client_id = (uint32_t)id;
    result->port = (uint16_t)port;
    strcpy(result->ip, tokens[3]);
    return true;
}

enum request_result client_search(ClientSession *client, const char *query)
{
    pthread_mutex_lock(&client->mutex);
    client->result_count = 0;
    enum request_result result = REQUEST_REJECTED;
    char token[FILE_NAME_TOKEN_CAP];
    if (!protocol_encode_filename(query, token, sizeof(token))) {
        fputs("Invalid search filename. Use a valid basename.\n", stderr);
        goto unlock;
    }
    if (!client->announced) {
        fputs("Log in before searching.\n", stderr);
        goto unlock;
    }
    char request[FILE_NAME_TOKEN_CAP + sizeof(CMD_SEARCH " " CRLF)];
    snprintf(request, sizeof(request), CMD_SEARCH " %s" CRLF, token);
    size_t length;
    if (!request_locked(client, request, &length)) {
        result = REQUEST_FAILED;
        goto unlock;
    }
    if (protocol_error(client->response, length)) {
        result = error_response_locked(client, client->response, length, REPLY_ALL);
        goto unlock;
    }
    size_t count = 0;
    while (!protocol_reply_is(client->response, length, STATUS_OK)) {
        FileSearchResult entry = {0};
        if (!parse_result(client->response, length, &entry) || strcmp(entry.name, query) != 0 ||
            (count != 0 && client->results[count - 1].client_id >= entry.client_id)) {
            result = fail_locked(client);
            goto unlock;
        }
        if (count == client->result_capacity) {
            size_t capacity = client->result_capacity;
            if (capacity > SIZE_MAX / sizeof(*client->results) / 2) {
                result = fail_locked(client);
                goto unlock;
            }
            size_t grown = capacity == 0 ? 8 : capacity * 2;
            FileSearchResult *results = realloc(client->results, grown * sizeof(*results));
            if (results == NULL) {
                result = fail_locked(client);
                goto unlock;
            }
            client->results = results;
            client->result_capacity = grown;
        }
        client->results[count++] = entry;
        if (read_response_locked(client, &length) != NET_LINE_OK) {
            result = REQUEST_FAILED;
            goto unlock;
        }
    }
    client->result_count = count;
    for (size_t i = 0; i < client->result_count; ++i) {
        FileSearchResult *entry = &client->results[i];
        printf("%zu: %s (client %" PRIu32 ", %s:%u)\n",
               i + 1, entry->name, entry->client_id, entry->ip, (unsigned)entry->port);
    }
    puts(REPLY_OK);
    result = REQUEST_OK;
unlock:
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_download(ClientSession *client, const char *number)
{
    static const char *const errors[] = {
        [DOWNLOAD_LOCAL_ERROR] = "LOCAL_ERROR",
        [DOWNLOAD_CONNECT_FAILED] = "CONNECT_FAILED",
        [DOWNLOAD_TIMEOUT] = "TIMEOUT",
        [DOWNLOAD_TRANSFER_FAILED] = "TRANSFER_FAILED",
        [DOWNLOAD_NOT_FOUND] = "NOT_FOUND",
        [DOWNLOAD_SIZE_MISMATCH] = "SIZE_MISMATCH"
    };
    pthread_mutex_lock(&client->mutex);
    uint64_t selection;
    if (!client->announced || number == NULL ||
        !protocol_parse_uint64(number, client->result_count, &selection) || selection == 0) {
        pthread_mutex_unlock(&client->mutex);
        fputs("Choose a valid result number from the latest search.\n", stderr);
        return REQUEST_REJECTED;
    }
    FileSearchResult entry = client->results[selection - 1];
    pthread_mutex_unlock(&client->mutex);
    enum download_result outcome = download_file(&entry, client->downloads, client->signals);
    if (outcome == DOWNLOAD_OK) {
        printf("Downloaded %s\n", entry.name);
        return REQUEST_OK;
    }
    if (outcome == DOWNLOAD_CANCELLED)
        return REQUEST_FAILED;
    fprintf(stderr, "Download %s from client %" PRIu32 " at %s:%u failed: %s (%s).\n",
            entry.name, entry.client_id, entry.ip, (unsigned)entry.port,
            errors[outcome], strerror(errno));
    return REQUEST_REJECTED;
}
