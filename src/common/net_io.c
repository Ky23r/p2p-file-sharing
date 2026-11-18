#include "net_io.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
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

int recv_line(net_reader *reader, char **line, size_t *capacity, size_t *length)
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
