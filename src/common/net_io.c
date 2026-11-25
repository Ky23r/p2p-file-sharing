#include "net_io.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

ssize_t send_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    size_t sent = 0;

    if (size > (size_t)SSIZE_MAX || (size != 0 && buffer == NULL)) {
        errno = EINVAL;
        return -1;
    }
    while (sent < size) {
        ssize_t n = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

int send_status(int fd, int status)
{
    char line[6];
    if (status < 100 || status > 599) {
        errno = EINVAL;
        return -1;
    }
    snprintf(line, sizeof(line), "%d\r\n", status);
    return send_all(fd, line, sizeof(line) - 1) < 0 ? -1 : 0;
}

int net_enable_keepalive(int fd)
{
    const int enabled = 1, idle_seconds = 60, interval_seconds = 10, probes = 3;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(idle_seconds)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_seconds, sizeof(interval_seconds)) < 0)
        return -1;
    return setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
}

void net_reader_init(net_reader *reader, int fd)
{
    reader->fd = fd;
    reader->next = 0;
    reader->used = 0;
}

ssize_t net_read(net_reader *reader, void *buffer, size_t size)
{
    if (reader == NULL || size > (size_t)SSIZE_MAX || (size != 0 && buffer == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;
    size_t available = reader->used - reader->next;
    if (available != 0) {
        size_t count = available < size ? available : size;
        memcpy(buffer, reader->buffer + reader->next, count);
        reader->next += count;
        return (ssize_t)count;
    }
    ssize_t count;
    do {
        count = recv(reader->fd, buffer, size, 0);
    } while (count < 0 && errno == EINTR);
    return count;
}

int recv_line_bounded(net_reader *reader, char **line, size_t *capacity, size_t *length,
                      size_t max_bytes_before_lf)
{
    size_t count = 0;

    for (;;) {
        if (reader->next == reader->used) {
            ssize_t n = recv(reader->fd, reader->buffer, sizeof(reader->buffer), 0);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return NET_LINE_ERROR;
            }
            if (n == 0)
                return count == 0 ? NET_LINE_EOF : NET_LINE_TRUNCATED;
            reader->next = 0;
            reader->used = (size_t)n;
        }
        const unsigned char *start = reader->buffer + reader->next;
        size_t available = reader->used - reader->next;
        const unsigned char *newline = memchr(start, '\n', available);
        size_t bytes = newline == NULL ? available : (size_t)(newline - start);
        if (bytes > max_bytes_before_lf - count)
            return NET_LINE_TOO_LONG;
        if (bytes >= SIZE_MAX - count) {
            errno = EOVERFLOW;
            return NET_LINE_ERROR;
        }
        size_t required = count + bytes + 1;
        if (required > *capacity) {
            size_t grown = *capacity <= SIZE_MAX / 2 ? *capacity * 2 : required;
            if (grown < required)
                grown = required;
            char *buffer = realloc(*line, grown);
            if (buffer == NULL)
                return NET_LINE_ERROR;
            *line = buffer;
            *capacity = grown;
        }
        memcpy(*line + count, start, bytes);
        count += bytes;
        reader->next += bytes;
        if (newline != NULL) {
            reader->next++;
            if (count == 0 || (*line)[count - 1] != '\r')
                return NET_LINE_INVALID;
            *length = count - 1;
            (*line)[*length] = '\0';
            return NET_LINE_OK;
        }
    }
}

int recv_line(net_reader *reader, char **line, size_t *capacity, size_t *length)
{
    return recv_line_bounded(reader, line, capacity, length, SIZE_MAX);
}
