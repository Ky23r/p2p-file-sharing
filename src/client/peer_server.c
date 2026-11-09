#include "peer_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int open_peer_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(fd, SOMAXCONN) == 0)
        return fd;

    int error = errno;
    close(fd);
    errno = error;
    return -1;
}
