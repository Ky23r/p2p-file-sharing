#include "transfer.h"
#include "net_io.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int download_temporary(int directory, char name[64])
{
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        unsigned char random[8];
        if (RAND_bytes(random, sizeof(random)) != 1) {
            errno = EIO;
            return -1;
        }
        strcpy(name, ".p2p-download-");
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(name + 14 + 2 * i, 3, "%02x", random[i]);
        int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}

static const char *receive_failure(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? "TIMEOUT" : "INTERRUPTED";
}

const char *download_file(const file_result *result, const char *directory)
{
    int folder = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0)
        return "LOCAL_ERROR";
    const char *failure = "LOCAL_ERROR";
    int peer = -1, output = -1;
    char temporary[64] = "";
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
    peer = file_connect(result->ip, result->port);
    if (peer < 0) {
        failure = errno == ETIMEDOUT || errno == EAGAIN ? "TIMEOUT" : "CONNECT_FAILED";
        goto finish;
    }
    char request[FILE_NAME_HEX_CAP + 8], encoded[FILE_NAME_HEX_CAP];
    file_hex_encode(result->name, encoded, sizeof(encoded));
    int length = snprintf(request, sizeof(request), "GET %s\n", encoded);
    if (send_all(peer, request, (size_t)length) < 0) {
        failure = receive_failure();
        goto finish;
    }
    net_reader reader;
    net_reader_init(&reader, peer);
    char header[PROTOCOL_MAX_LINE + 1], *tokens[3];
    size_t header_length;
    int status = recv_line(&reader, header, sizeof(header), &header_length);
    if (status != NET_LINE_OK) {
        failure = status == NET_LINE_ERROR ? receive_failure() : "INVALID_RESPONSE";
        if (status != NET_LINE_ERROR)
            errno = EPROTO;
        goto finish;
    }
    if (file_error(header, header_length)) {
        failure = "PEER_ERROR";
        errno = EPROTO;
        goto finish;
    }
    uint64_t size;
    if (!file_tokens(header, header_length, tokens, 3) || strcmp(tokens[0], "OK") != 0 ||
        strcmp(tokens[1], "FILE") != 0 || !file_decimal(tokens[2], FILE_SIZE_MAX, &size)) {
        failure = "INVALID_RESPONSE";
        errno = EPROTO;
        goto finish;
    }
    if (size != result->size) {
        failure = "SIZE_MISMATCH";
        errno = EPROTO;
        goto finish;
    }
    unsigned char buffer[65536];
    uint64_t remaining = size;
    while (remaining != 0) {
        size_t amount = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t received = net_read(&reader, buffer, amount);
        if (received <= 0) {
            failure = received == 0 ? "INTERRUPTED" : receive_failure();
            if (received == 0)
                errno = ECONNRESET;
            goto finish;
        }
        size_t written = 0;
        while (written < (size_t)received) {
            ssize_t count = write(output, buffer + written, (size_t)received - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                failure = "LOCAL_ERROR";
                goto finish;
            }
            written += (size_t)count;
        }
        remaining -= (uint64_t)received;
    }
    ssize_t extra = net_read(&reader, buffer, 1);
    if (extra != 0) {
        failure = extra > 0 ? "SIZE_MISMATCH" : receive_failure();
        if (extra > 0)
            errno = EPROTO;
        goto finish;
    }
    failure = "LOCAL_ERROR";
    if (fsync(output) != 0)
        goto finish;
    int closed = close(output);
    output = -1;
    if (closed != 0 || linkat(folder, temporary, folder, result->name, 0) != 0)
        goto finish;
    failure = NULL;
finish:;
    int error = errno;
    if (output >= 0)
        close(output);
    if (peer >= 0)
        close(peer);
    if (*temporary)
        unlinkat(folder, temporary, 0);
    close(folder);
    errno = error;
    return failure;
}
