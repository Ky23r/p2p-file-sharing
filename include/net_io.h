#ifndef P2P_NET_IO_H
#define P2P_NET_IO_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    unsigned char buffer[4096];
    size_t next;
    size_t used;
} net_reader;

enum net_line_result {
    NET_LINE_ERROR = -1,       /* errno from recv, or EINVAL */
    NET_LINE_TOO_LONG = -2,    /* close stream; do not reuse reader */
    NET_LINE_TRUNCATED = -3,   /* EOF with bytes but no LF */
    NET_LINE_EOF = 0,          /* EOF between lines */
    NET_LINE_OK = 1
};

/* Blocking sockets. Retry EINTR; suppress SIGPIPE. Return size or -1/errno.
 * Failure can occur after a partial send; caller must close the connection.
 */
ssize_t send_all(int fd, const void *buffer, size_t size);

void net_reader_init(net_reader *reader, int fd);

/* capacity includes trailing NUL; length excludes LF and NUL. Embedded NULs
 * are preserved and counted: callers must validate bytes using length.
 * Preserves bytes after LF for the next call. EOF never yields a partial line.
 * On TOO_LONG/ERROR/TRUNCATED, close the stream. One reader per connection;
 * no concurrent calls on the same reader or reads outside it.
 */
int recv_line(net_reader *reader, char *line, size_t capacity, size_t *length);

#endif
