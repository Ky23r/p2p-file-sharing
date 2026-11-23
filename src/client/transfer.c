#include "transfer.h"
#include "net_io.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOWNLOAD_TEMP_ATTEMPTS 8U
#define DOWNLOAD_TEMP_RANDOM_BYTES 8U

static const char download_temp_prefix[] = ".p2p-download-";
#define DOWNLOAD_TEMP_NAME_CAP (sizeof(download_temp_prefix) + 2U * DOWNLOAD_TEMP_RANDOM_BYTES)

static int download_temporary(int directory, char name[DOWNLOAD_TEMP_NAME_CAP])
{
    for (unsigned attempt = 0; attempt < DOWNLOAD_TEMP_ATTEMPTS; ++attempt) {
        unsigned char random[DOWNLOAD_TEMP_RANDOM_BYTES];
        if (RAND_bytes(random, sizeof(random)) != 1) {
            errno = EIO;
            return -1;
        }
        memcpy(name, download_temp_prefix, sizeof(download_temp_prefix));
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(name + sizeof(download_temp_prefix) - 1 + 2 * i, 3, "%02x", random[i]);
        int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}

static const char *receive_failure(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? "TIMEOUT" : "TRANSFER_FAILED";
}

static const char *receive_line_failure(int status)
{
    if (status == NET_LINE_ERROR)
        return receive_failure();
    errno = EPROTO;
    return "TRANSFER_FAILED";
}

static const char *receive_payload(net_reader *reader, int output, size_t length)
{
    unsigned char buffer[FILE_CHUNK_MAX];
    size_t received = 0;
    while (received < length) {
        ssize_t count = net_read(reader, buffer + received, length - received);
        if (count < 0)
            return receive_failure();
        if (count == 0) {
            errno = ECONNRESET;
            return "TRANSFER_FAILED";
        }
        received += (size_t)count;
    }
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(output, buffer + written, length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count == 0)
                errno = EIO;
            return "LOCAL_ERROR";
        }
        written += (size_t)count;
    }
    return NULL;
}

const char *download_file(const FileSearchResult *result, const char *directory, ClientSignals *signals)
{
    char token[FILE_NAME_TOKEN_CAP];
    if (!protocol_encode_filename(result->name, token, sizeof(token))) {
        errno = EINVAL;
        return "LOCAL_ERROR";
    }
    int folder = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0)
        return "LOCAL_ERROR";
    const char *failure = "LOCAL_ERROR";
    int peer = -1, output = -1;
    char *header = NULL;
    size_t capacity = 0;
    char temporary[DOWNLOAD_TEMP_NAME_CAP] = "";
    struct stat existing;
    if (fstatat(folder, result->name, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        goto finish;
    }
    if (errno != ENOENT)
        goto finish;
    output = download_temporary(folder, temporary);
    if (output < 0) {
        temporary[0] = '\0'; /* An O_EXCL failure never authorizes removing a file. */
        goto finish;
    }
    peer = file_connect(result->ip, result->port, signals);
    if (peer < 0) {
        failure = errno == ETIMEDOUT || errno == EAGAIN ? "TIMEOUT" : "CONNECT_FAILED";
        goto finish;
    }
    char request[FILE_NAME_TOKEN_CAP + sizeof("GET " CRLF)];
    int length = snprintf(request, sizeof(request), "GET %s" CRLF, token);
    if (send_all(peer, request, (size_t)length) < 0) {
        failure = receive_failure();
        goto finish;
    }
    net_reader reader;
    net_reader_init(&reader, peer);
    char *tokens[2];
    size_t header_length;
    int status = recv_line_bounded(&reader, &header, &capacity, &header_length, FILE_PEER_LINE_MAX);
    if (status != NET_LINE_OK) {
        failure = receive_line_failure(status);
        goto finish;
    }
    if (protocol_error(header, header_length)) {
        errno = EPROTO;
        if (strcmp(header, STATUS_NOT_FOUND) == 0) {
            failure = "NOT_FOUND";
            errno = ENOENT;
        } else {
            failure = "TRANSFER_FAILED";
        }
        goto finish;
    }
    if (protocol_tokens(header, header_length, tokens, 2) != 2 || strcmp(tokens[0], STATUS_OK) != 0 ||
        strcmp(tokens[1], FILE_TRANSFER_MODE) != 0) {
        failure = "TRANSFER_FAILED";
        errno = EPROTO;
        goto finish;
    }
    for (;;) {
        status = recv_line_bounded(&reader, &header, &capacity, &header_length, FILE_PEER_LINE_MAX);
        if (status != NET_LINE_OK) {
            failure = receive_line_failure(status);
            goto finish;
        }
        uint64_t amount;
        if (protocol_tokens(header, header_length, tokens, 2) != 2 ||
            !protocol_parse_uint64(tokens[1], FILE_CHUNK_MAX, &amount)) {
            failure = "TRANSFER_FAILED";
            errno = EPROTO;
            goto finish;
        }
        bool final = strcmp(tokens[0], FILE_END_TAG) == 0;
        if (!final && (strcmp(tokens[0], FILE_DATA_TAG) != 0 || amount == 0)) {
            failure = "TRANSFER_FAILED";
            errno = EPROTO;
            goto finish;
        }
        failure = receive_payload(&reader, output, (size_t)amount);
        if (failure != NULL)
            goto finish;
        if (final)
            break;
    }
    failure = "LOCAL_ERROR";
    if (client_signals_number(signals) != 0)
        goto finish;
    if (fsync(output) != 0)
        goto finish;
    int closed = close(output);
    output = -1;
    if (closed != 0 || linkat(folder, temporary, folder, result->name, 0) != 0)
        goto finish;
    failure = NULL;
finish:;
    int error = errno;
    free(header);
    if (failure != NULL && client_signals_number(signals) != 0) {
        failure = "CANCELLED";
        error = ECANCELED;
    }
    if (output >= 0)
        close(output);
    if (peer >= 0)
        client_signals_close_socket(signals, peer);
    if (*temporary)
        unlinkat(folder, temporary, 0);
    close(folder);
    errno = error;
    return failure;
}
