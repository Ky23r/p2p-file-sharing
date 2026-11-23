#include "peer_server.h"
#include "net_io.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UPLOAD_WORKERS 4U

typedef struct {
    struct PeerServer *server;
    pthread_t thread;
    int fd;
} UploadWorker;

struct PeerServer {
    int listener;
    int stop[2];
    pthread_mutex_t mutex;
    bool stopping;
    size_t started;
    SharedIndex *index;
    UploadWorker workers[UPLOAD_WORKERS];
};

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

static void peer_error(int fd, const char *code)
{
    (void)send_all(fd, code, strlen(code));
}

static ssize_t read_chunk(int source, unsigned char buffer[FILE_CHUNK_MAX])
{
    ssize_t count;
    do {
        count = read(source, buffer, FILE_CHUNK_MAX);
    } while (count < 0 && errno == EINTR);
    return count;
}

static void send_file(int peer, int source)
{
    const char response[] = STATUS_OK " " FILE_TRANSFER_MODE CRLF;
    if (send_all(peer, response, sizeof(response) - 1) < 0)
        return;

    /* Read one chunk ahead to mark the last payload using the source's EOF. */
    unsigned char chunks[2][FILE_CHUNK_MAX];
    size_t current = 0;
    ssize_t bytes = read_chunk(source, chunks[current]);
    if (bytes < 0)
        return;
    for (;;) {
        size_t next = 1 - current;
        ssize_t following = bytes == 0 ? 0 : read_chunk(source, chunks[next]);
        if (following < 0)
            return;
        bool final = following == 0;
        char header[32];
        int length = snprintf(header, sizeof(header), "%s %zd" CRLF,
                          final ? FILE_END_TAG : FILE_DATA_TAG, bytes);
        if (send_all(peer, header, (size_t)length) < 0 ||
            send_all(peer, chunks[current], (size_t)bytes) < 0 || final)
            return;
        current = next;
        bytes = following;
    }
}

static void serve_file(int fd, SharedIndex *index)
{
    net_reader reader;
    net_reader_init(&reader, fd);
    char *line = NULL;
    size_t capacity = 0, length;
    int received = recv_line_bounded(&reader, &line, &capacity, &length, FILE_PEER_LINE_MAX);
    char *tokens[2], name[FILE_NAME_CAP];
    const char *error = NULL;
    if (received == NET_LINE_INVALID || received == NET_LINE_TOO_LONG ||
        (received == NET_LINE_OK &&
         (protocol_tokens(line, length, tokens, 2) != 2 ||
          !protocol_decode_filename(tokens[1], name, sizeof(name)))))
        error = STATUS_BAD_REQUEST CRLF;
    else if (received == NET_LINE_OK && strcmp(tokens[0], "GET") != 0)
        error = STATUS_UNKNOWN_COMMAND CRLF;
    free(line);
    if (error != NULL) {
        peer_error(fd, error);
        return;
    }
    if (received != NET_LINE_OK)
        return;
    int source = shared_index_open(index, name);
    if (source < 0) {
        peer_error(fd, STATUS_NOT_FOUND CRLF);
        return;
    }
    send_file(fd, source);
    close(source);
}

static void *upload_loop(void *argument)
{
    UploadWorker *worker = argument;
    PeerServer *server = worker->server;
    struct pollfd descriptors[2] = {
        {server->listener, POLLIN, 0}, {server->stop[0], POLLIN, 0}
    };
    for (;;) {
        int ready = poll(descriptors, 2, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || descriptors[1].revents != 0 ||
            (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
            break;
        int fd = accept(server->listener, NULL, NULL);
        if (fd < 0)
            continue; /* Another worker can win the nonblocking accept. */
        pthread_mutex_lock(&server->mutex);
        if (server->stopping) {
            close(fd);
            pthread_mutex_unlock(&server->mutex);
            break;
        }
        worker->fd = fd;
        pthread_mutex_unlock(&server->mutex);
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && file_socket_timeout(fd) == 0)
            serve_file(fd, server->index);
        pthread_mutex_lock(&server->mutex);
        close(fd);
        worker->fd = -1;
        pthread_mutex_unlock(&server->mutex);
    }
    return NULL;
}

PeerServer *peer_server_start(int listener, SharedIndex *index)
{
    PeerServer *server = calloc(1, sizeof(*server));
    if (server == NULL)
        return NULL;
    server->listener = listener;
    server->index = index;
    int error = pthread_mutex_init(&server->mutex, NULL);
    if (error != 0) {
        free(server);
        errno = error;
        return NULL;
    }
    int flags = fcntl(listener, F_GETFL);
    if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) != 0 || pipe(server->stop) != 0) {
        error = errno;
        pthread_mutex_destroy(&server->mutex);
        free(server);
        errno = error;
        return NULL;
    }
    if (fcntl(server->stop[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(server->stop[1], F_SETFD, FD_CLOEXEC) < 0) {
        error = errno;
        peer_server_stop(server);
        errno = error;
        return NULL;
    }
    sigset_t previous_mask;
    if (client_signals_block_workers(&previous_mask) != 0) {
        error = errno;
        peer_server_stop(server);
        errno = error;
        return NULL;
    }
    for (size_t i = 0; i < UPLOAD_WORKERS; ++i) {
        server->workers[i].server = server;
        server->workers[i].fd = -1;
        error = pthread_create(&server->workers[i].thread, NULL, upload_loop, &server->workers[i]);
        if (error != 0) {
            peer_server_stop(server);
            pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
            errno = error;
            return NULL;
        }
        server->started++;
    }
    error = pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (error != 0) {
        peer_server_stop(server);
        errno = error;
        return NULL;
    }
    return server;
}

void peer_server_stop(PeerServer *server)
{
    if (server == NULL)
        return;
    pthread_mutex_lock(&server->mutex);
    server->stopping = true;
    for (size_t i = 0; i < server->started; ++i)
        if (server->workers[i].fd >= 0)
            shutdown(server->workers[i].fd, SHUT_RDWR);
    pthread_mutex_unlock(&server->mutex);
    char stop = 1;
    ssize_t written;
    do {
        written = write(server->stop[1], &stop, 1);
    } while (written < 0 && errno == EINTR);
    for (size_t i = 0; i < server->started; ++i)
        pthread_join(server->workers[i].thread, NULL);
    close(server->stop[0]);
    close(server->stop[1]);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}
