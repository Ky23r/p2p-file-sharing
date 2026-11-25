#define _GNU_SOURCE
#include "client_session.h"
#include "net_io.h"
#include "protocol.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UNSHARE_RETRY_MS 10000

typedef struct PendingUnshare {
    struct PendingUnshare *next;
    char token[FILE_NAME_TOKEN_CAP];
} PendingUnshare;

struct ClientSession {
    net_reader server;
    char *response;
    size_t response_capacity;
    pthread_mutex_t mutex;
    pthread_t monitor;
    int failure[2];
    atomic_bool stopping;
    atomic_bool failed;
    bool announced;
    uint32_t id;
    SharedIndex *index;
    PendingUnshare *withdrawals;
    const char *downloads;
    ClientSignals *signals;
    size_t result_count;
    size_t result_capacity;
    FileSearchResult *results;
    char search_name[FILE_NAME_CAP];
};

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
    client->announced = false;
    client->result_count = 0;
    while (client->withdrawals != NULL)
        remove_withdrawal_locked(&client->withdrawals);
    shared_index_set_online(client->index, false);
}

static void close_failed_session_locked(ClientSession *client)
{
    if (client->failed)
        return;
    client->failed = true;
    deactivate_locked(client);
    shutdown(client->server.fd, SHUT_RDWR);
    char notification = 1;
    ssize_t written;
    do {
        written = write(client->failure[1], &notification, 1);
    } while (written < 0 && errno == EINTR);
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
    int status = recv_line_bounded(&client->server, &client->response, &client->response_capacity,
                                   length, CONTROL_LINE_MAX);
    if (status != NET_LINE_OK)
        fail_locked(client);
    return status;
}

static enum request_result error_response_locked(ClientSession *client, int status, FILE *output)
{
    if (status < 400 || status > 599)
        return fail_locked(client);
    const char *message;
    switch (status) {
    case STATUS_BAD_REQUEST: message = "Invalid request"; break;
    case STATUS_UNAUTHORIZED: message = "Login required or invalid credentials"; break;
    case STATUS_FORBIDDEN: message = "Request not allowed"; break;
    case STATUS_NOT_FOUND: message = "File unavailable"; break;
    case STATUS_CONFLICT: message = "Account or session conflict"; break;
    case STATUS_INTERNAL_ERROR: message = "Server error"; break;
    case STATUS_UNAVAILABLE: message = "Service unavailable"; break;
    default: message = "Request failed"; break;
    }
    fprintf(output, "%s\n", message);
    if (status == STATUS_UNAVAILABLE) {
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

static enum request_result exchange_locked(ClientSession *client, const char *request,
                                           FILE *output)
{
    size_t length;
    if (!request_locked(client, request, &length))
        return REQUEST_FAILED;
    int status = protocol_status(client->response, length);
    return status == STATUS_OK ? REQUEST_OK : error_response_locked(client, status, output);
}

static enum request_result report_failure_locked(ClientSession *client, uint32_t id,
                                                 const char *name, bool stale)
{
    char token[FILE_NAME_TOKEN_CAP];
    if (!protocol_encode_filename(name, token, sizeof(token)))
        return REQUEST_REJECTED;
    char request[FILE_NAME_TOKEN_CAP + 32];
    snprintf(request, sizeof(request), CMD_FAIL " %" PRIu32 " %s %d" CRLF, id, token,
             stale ? STATUS_NOT_FOUND : STATUS_UNAVAILABLE);
    return exchange_locked(client, request, stderr);
}

static enum request_result withdraw_locked(ClientSession *client, PendingUnshare **link, FILE *output)
{
    char request[FILE_NAME_TOKEN_CAP + sizeof(CMD_UNSHARE " " CRLF)];
    snprintf(request, sizeof(request), CMD_UNSHARE " %s" CRLF, (*link)->token);
    enum request_result result = exchange_locked(client, request, output);
    if (result == REQUEST_OK)
        remove_withdrawal_locked(link);
    return result;
}

static enum request_result retry_withdrawals_locked(ClientSession *client)
{
    PendingUnshare **link = &client->withdrawals;
    while (*link != NULL) {
        enum request_result result = withdraw_locked(client, link, stderr);
        if (result == REQUEST_FAILED)
            return result;
        if (result == REQUEST_REJECTED)
            link = &(*link)->next;
    }
    return REQUEST_OK;
}

static enum request_result publish_locked(ClientSession *client, const SharedFile *entry)
{
    char token[FILE_NAME_TOKEN_CAP];
    if (!protocol_encode_filename(entry->name, token, sizeof(token))) {
        fprintf(stderr, "Share %s remains pending: invalid filename.\n", entry->name);
        return REQUEST_REJECTED;
    }
    int source = shared_file_open(entry->path);
    if (source < 0) {
        int error = errno;
        fprintf(stderr, "Cannot share %s: %s.\n", entry->name, strerror(error));
        if (entry->announced && shared_file_stale(error)) {
            enum request_result result = report_failure_locked(client, client->id, entry->name, true);
            if (result == REQUEST_FAILED)
                return result;
            if (result == REQUEST_OK)
                shared_index_mark(client->index, entry->name, false);
        }
        return REQUEST_REJECTED;
    }
    close(source);
    char request[FILE_NAME_TOKEN_CAP + 32];
    snprintf(request, sizeof(request), CMD_SHARE " %" PRIu32 " %s" CRLF, client->id, token);
    enum request_result result = exchange_locked(client, request, stdout);
    if (result == REQUEST_OK) {
        PendingUnshare **withdrawal = find_withdrawal_locked(client, token);
        if (*withdrawal != NULL)
            remove_withdrawal_locked(withdrawal);
        shared_index_mark(client->index, entry->name, true);
        puts("Shared");
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

static void *monitor_control(void *argument)
{
    ClientSession *client = argument;
    struct pollfd control = {client->server.fd, POLLRDHUP, 0};
    while (!client->stopping && !client->failed) {
        /* Only exchanges read the socket. A poll timeout services queued
         * withdrawals, whose retries need no peer announcement. */
        int ready = poll(&control, 1, UNSHARE_RETRY_MS);
        if (ready < 0 && errno == EINTR)
            continue;
        pthread_mutex_lock(&client->mutex);
        if (!client->stopping && !client->failed) {
            if (ready != 0)
                fail_locked(client);
            else
                (void)retry_withdrawals_locked(client);
        }
        pthread_mutex_unlock(&client->mutex);
    }
    return NULL;
}

ClientSession *client_session_start(int fd, uint32_t id,
                                     SharedIndex *index, const char *downloads, ClientSignals *signals)
{
    if (net_enable_keepalive(fd) != 0)
        return NULL;
    ClientSession *client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    atomic_init(&client->stopping, false);
    atomic_init(&client->failed, false);
    net_reader_init(&client->server, fd);
    client->id = id;
    client->index = index;
    client->downloads = downloads;
    client->signals = signals;
    int error = pthread_mutex_init(&client->mutex, NULL);
    if (error != 0)
        goto free_client;
    if (pipe2(client->failure, O_CLOEXEC) != 0) {
        error = errno;
        goto destroy_mutex;
    }
    sigset_t previous_mask;
    if (client_signals_block_workers(&previous_mask) != 0) {
        error = errno;
        goto close_failure;
    }
    error = pthread_create(&client->monitor, NULL, monitor_control, client);
    int mask_error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error == 0 && mask_error != 0) {
        client_session_stop(client);
        errno = mask_error;
        return NULL;
    }
    if (error == 0)
        return client;
close_failure:
    close(client->failure[0]);
    close(client->failure[1]);
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
    pthread_join(client->monitor, NULL);
    pthread_mutex_lock(&client->mutex);
    deactivate_locked(client);
    pthread_mutex_unlock(&client->mutex);
    close(client->failure[0]);
    close(client->failure[1]);
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

enum request_result client_stage_hello(ClientSession *client, uint16_t port)
{
    char request[64];
    snprintf(request, sizeof(request), CMD_HELLO " %" PRIu32 " %u" CRLF, client->id, (unsigned)port);
    pthread_mutex_lock(&client->mutex);
    enum request_result result = exchange_locked(client, request, stdout);
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_account(ClientSession *client, char *request, size_t capacity,
                                   bool registering)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_REJECTED;
    if (strlen(request) <= CONTROL_LINE_MAX + 1U)
        result = exchange_locked(client, request, stdout);
    else
        fputs("Account request is too long.\n", stderr);
    OPENSSL_cleanse(request, capacity);
    if (result == REQUEST_OK) {
        puts(registering ? "Registered" : "Logged in");
        if (!registering) {
            client->announced = true;
            shared_index_set_online(client->index, true);
            result = republish_locked(client);
        }
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_logout(ClientSession *client)
{
    pthread_mutex_lock(&client->mutex);
    enum request_result result = exchange_locked(client, CMD_LOGOUT CRLF, stdout);
    if (result == REQUEST_OK) {
        deactivate_locked(client);
        puts("Logged out");
    }
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_share(ClientSession *client, const char *path)
{
    SharedFile entry;
    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_REJECTED;
    if (path == NULL || shared_index_add(client->index, path, &entry) != 0) {
        fprintf(stderr, "Cannot share file: %s.\n", path == NULL ? "missing path" : strerror(errno));
    } else if (client->announced) {
        result = publish_locked(client, &entry);
    } else {
        printf("Saved %s\n", entry.name);
        result = REQUEST_OK;
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
        result = withdraw_locked(client, withdrawal, stdout);
        if (result == REQUEST_REJECTED)
            fprintf(stderr, "Removed %s locally. Server withdrawal will be retried.\n", name);
    }
    if (result == REQUEST_OK)
        puts("Unshared");
unlock:
    pthread_mutex_unlock(&client->mutex);
    return result;
}

void client_report_upload_failure(void *context, const char *name, bool check_stale)
{
    ClientSession *client = context;
    pthread_mutex_lock(&client->mutex);
    if (client->announced && !client->stopping) {
        bool stale = false;
        if (check_stale) {
            /* The source may have been repaired or republished since the failed GET. */
            int source = shared_index_open(client->index, name);
            if (source < 0)
                stale = shared_file_stale(errno);
            else
                close(source);
        }
        if (report_failure_locked(client, client->id, name, stale) == REQUEST_OK && stale)
            shared_index_mark(client->index, name, false);
    }
    pthread_mutex_unlock(&client->mutex);
}

void client_shares(ClientSession *client)
{
    SharedFile entry;
    for (size_t i = 0; shared_index_get(client->index, i, &entry); ++i)
        printf("%s (%s)\n", entry.name, entry.announced ? "shared" : "pending");
}

static bool parse_result(char *line, size_t length, FileSearchResult *result)
{
    char *tokens[3];
    uint64_t id, port;
    struct in_addr address;
    if (protocol_tokens(line, length, tokens, 3) != 3 ||
        !protocol_parse_uint64(tokens[0], UINT32_MAX, &id) ||
        inet_pton(AF_INET, tokens[1], &address) != 1 ||
        !protocol_parse_uint64(tokens[2], UINT16_MAX, &port) || port == 0)
        return false;
    result->client_id = (uint32_t)id;
    result->port = (uint16_t)port;
    strcpy(result->ip, tokens[1]);
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
    if (length == 3) {
        result = error_response_locked(client, protocol_status(client->response, length), stdout);
        goto unlock;
    }
    char *fields[2];
    uint64_t count;
    if (protocol_tokens(client->response, length, fields, 2) != 2 ||
        protocol_status(fields[0], strlen(fields[0])) != STATUS_OK ||
        !protocol_parse_uint64(fields[1], SIZE_MAX / sizeof(*client->results), &count)) {
        result = fail_locked(client);
        goto unlock;
    }
    for (size_t i = 0; i < count; ++i) {
        if (read_response_locked(client, &length) != NET_LINE_OK) {
            result = REQUEST_FAILED;
            goto unlock;
        }
        FileSearchResult entry = {0};
        if (!parse_result(client->response, length, &entry) || entry.client_id == client->id ||
            (i != 0 && client->results[i - 1].client_id >= entry.client_id)) {
            result = fail_locked(client);
            goto unlock;
        }
        if (i == client->result_capacity) {
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
        client->results[i] = entry;
    }
    client->result_count = count;
    strcpy(client->search_name, query);
    printf("Found %zu\n", client->result_count);
    for (size_t i = 0; i < client->result_count; ++i) {
        FileSearchResult *entry = &client->results[i];
        printf("%zu: %s (client %" PRIu32 ", %s:%u)\n",
               i + 1, query, entry->client_id, entry->ip, (unsigned)entry->port);
    }
    result = REQUEST_OK;
unlock:
    pthread_mutex_unlock(&client->mutex);
    return result;
}

enum request_result client_download(ClientSession *client, const char *number)
{
    static const char *const errors[] = {
        [DOWNLOAD_LOCAL_ERROR] = "local file error",
        [DOWNLOAD_CONNECT_FAILED] = "could not connect",
        [DOWNLOAD_TIMEOUT] = "timed out",
        [DOWNLOAD_TRANSFER_FAILED] = "transfer failed",
        [DOWNLOAD_NOT_FOUND] = "file unavailable"
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
    char name[FILE_NAME_CAP];
    strcpy(name, client->search_name);
    pthread_mutex_unlock(&client->mutex);
    enum download_result outcome = download_file(&entry, name, client->downloads, client->signals);
    if (outcome == DOWNLOAD_OK) {
        printf("Downloaded %s\n", name);
        return REQUEST_OK;
    }
    if (outcome == DOWNLOAD_CANCELLED)
        return REQUEST_FAILED;
    fprintf(stderr, "Download %s: %s (%s).\n", name, errors[outcome], strerror(errno));
    if (outcome == DOWNLOAD_LOCAL_ERROR)
        return REQUEST_REJECTED;

    pthread_mutex_lock(&client->mutex);
    enum request_result result = REQUEST_REJECTED;
    if (client->announced && !client->stopping)
        result = report_failure_locked(client, entry.client_id, name, false);
    pthread_mutex_unlock(&client->mutex);
    return result == REQUEST_FAILED ? REQUEST_FAILED : REQUEST_REJECTED;
}
