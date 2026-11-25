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
    NET_LINE_INVALID = -2,    /* bare LF; next line can still be read */
    NET_LINE_TRUNCATED = -3,  /* EOF during a line */
    NET_LINE_TOO_LONG = -4,   /* line limit exceeded; close the stream */
    NET_LINE_EOF = 0,          /* EOF between lines */
    NET_LINE_OK = 1
};

/* Blocking sockets. Retry EINTR; suppress SIGPIPE. Return size or -1/errno.
 * Failure can occur after a partial send; caller must close the connection.
 */
ssize_t send_all(int fd, const void *buffer, size_t size);
/* Send a three-digit status followed by CRLF. Returns 0 or -1/errno. */
int send_status(int fd, int status);

/* Enable TCP keepalive on a control socket. Returns 0 or -1/errno. */
int net_enable_keepalive(int fd);

void net_reader_init(net_reader *reader, int fd);

/* Pass a NULL line and zero capacity initially. Reuses/grows the caller-owned
 * buffer, which the caller must free. Length excludes CRLF and the final NUL.
 * Embedded NULs are counted. On ERROR/TRUNCATED, close the stream.
 * Use the same reader for all reads on a connection.
 */
int recv_line(net_reader *reader, char **line, size_t *capacity, size_t *length);

/* Like recv_line, with a limit on bytes before LF (including the CR). */
int recv_line_bounded(net_reader *reader, char **line, size_t *capacity, size_t *length,
                      size_t max_bytes_before_lf);

/* Read up to size binary bytes, consuming prefetched bytes first. Like recv:
 * positive byte count, 0 at EOF (or size == 0), -1/errno on failure. Retry EINTR.
 * Use the same reader for headers and bodies; never bypass its buffer.
 */
ssize_t net_read(net_reader *reader, void *buffer, size_t size);

#endif
