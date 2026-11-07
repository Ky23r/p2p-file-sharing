#include "net_io.h"

#include <errno.h>
#include <limits.h>
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

int recv_line(net_reader *reader, char *line, size_t capacity, size_t *length)
{
    size_t count = 0;

    if (reader == NULL || line == NULL || length == NULL || capacity == 0) {
        errno = EINVAL;
        return NET_LINE_ERROR;
    }
    *length = 0;
    line[0] = '\0';
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
        unsigned char byte = reader->buffer[reader->next++];
        if (byte == '\n') {
            *length = count;
            return NET_LINE_OK;
        }
        if (count == capacity - 1)
            return NET_LINE_TOO_LONG;
        line[count++] = (char)byte;
        line[count] = '\0';
    }
}
