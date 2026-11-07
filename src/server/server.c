#include "auth.h"
#include "net_io.h"
#include "protocol.h"
#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_ACCOUNTS "data/server/accounts.db"
#define SOCKET_TIMEOUT_SECONDS 30

typedef struct {
    auth_store auth;
    session_manager sessions;
} server_state;

typedef struct {
    server_state *server;
    session *client;
} worker_args;

/* Only the signal handler needs process-global state. Nonblocking self-pipe
 * wakes poll; a full pipe already contains a pending shutdown notification.
 */
static volatile sig_atomic_t signal_fd = -1;

static void on_shutdown(int signal_number)
{
    int saved = errno;
    unsigned char byte = (unsigned char)signal_number;
    int fd = (int)signal_fd;
    if (fd >= 0) {
        ssize_t ignored = write(fd, &byte, 1);
        (void)ignored;
    }
    errno = saved;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--bind IPv4] [--port 1..65535] [--accounts PATH]\n"
            "       %s --help\n"
            "Defaults: %s:%u, accounts: %s\n"
            "Maximum clients: %u; socket inactivity timeout: %d seconds.\n"
            "SIGINT/SIGTERM gracefully stop the server.\n"
            "Plaintext authentication is for controlled local testing only.\n",
            program, program, DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT,
            DEFAULT_ACCOUNTS, SESSION_LIMIT, SOCKET_TIMEOUT_SECONDS);
}

static bool decimal(const char *text, uint32_t maximum, uint32_t *value)
{
    uint32_t result = 0;
    if (*text == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        uint32_t digit = (uint32_t)(*p - '0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10))
            return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

static const char *auth_response(enum auth_result result, bool registering)
{
    switch (result) {
    case AUTH_OK: return registering ? "OK REGISTERED\n" : "OK LOGGED_IN\n";
    case AUTH_INVALID: return "ERR INVALID_REQUEST\n";
    case AUTH_USER_EXISTS: return "ERR USER_EXISTS\n";
    case AUTH_BAD_CREDENTIALS: return "ERR INVALID_CREDENTIALS\n";
    case AUTH_ERROR: return "ERR INTERNAL_ERROR\n";
    }
    return "ERR INTERNAL_ERROR\n";
}

static const char *dispatch(server_state *server, session *client, char *line, size_t length)
{
    char *args[3] = {line, NULL, NULL};
    size_t count = 1;
    if (length == 0 || line[0] == ' ' || line[length - 1] == ' ')
        return "ERR INVALID_REQUEST\n";
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c > 0x7e)
            return "ERR INVALID_REQUEST\n";
        if (c == ' ') {
            if (count == 3 || line[i + 1] == ' ')
                return "ERR INVALID_REQUEST\n";
            args[count++] = line + i + 1;
        }
    }
    /* Validate the complete byte sequence before introducing token NULs. */
    for (size_t i = 1; i < count; ++i)
        args[i][-1] = '\0';

    if (strcmp(args[0], "REGISTER") == 0 || strcmp(args[0], "LOGIN") == 0) {
        if (count != 3 || !auth_valid_username(args[1]) || !auth_valid_password(args[2]))
            return "ERR INVALID_REQUEST\n";
        if (session_authenticated(&server->sessions, client))
            return "ERR ALREADY_AUTHENTICATED\n";
        bool registering = strcmp(args[0], "REGISTER") == 0;
        enum auth_result result = registering ? auth_register(&server->auth, args[1], args[2]) :
                                               auth_login(&server->auth, args[1], args[2]);
        if (!registering && result == AUTH_OK)
            session_login(&server->sessions, client, args[1]);
        return auth_response(result, registering);
    }
    if (strcmp(args[0], "HELLO") == 0) {
        uint32_t id, port;
        if (count != 3 || !decimal(args[1], UINT32_MAX, &id) ||
            !decimal(args[2], UINT16_MAX, &port) || port == 0)
            return "ERR INVALID_REQUEST\n";
        enum session_result result = session_hello(&server->sessions, client, id, (uint16_t)port);
        if (result == SESSION_UNAUTHORIZED)
            return "ERR AUTH_REQUIRED\n";
        if (result == SESSION_ID_IN_USE)
            return "ERR CLIENT_ID_IN_USE\n";
        return "OK HELLO\n";
    }
    if (strcmp(args[0], "LOGOUT") == 0) {
        if (count != 1)
            return "ERR INVALID_REQUEST\n";
        return session_logout(&server->sessions, client) ? "OK LOGGED_OUT\n" : "ERR AUTH_REQUIRED\n";
    }
    return "ERR UNKNOWN_COMMAND\n";
}

static void *client_worker(void *argument)
{
    worker_args args = *(worker_args *)argument;
    free(argument);
    net_reader reader;
    char line[PROTOCOL_MAX_LINE + 1];
    size_t length;
    net_reader_init(&reader, args.client->fd);
    for (;;) {
        int result = recv_line(&reader, line, sizeof(line), &length);
        if (result != NET_LINE_OK) {
            if (result == NET_LINE_TOO_LONG) {
                static const char response[] = "ERR LINE_TOO_LONG\n";
                (void)send_all(args.client->fd, response, sizeof(response) - 1);
            }
            break;
        }
        const char *response = dispatch(args.server, args.client, line, length);
        OPENSSL_cleanse(line, sizeof(line));
        if (send_all(args.client->fd, response, strlen(response)) < 0)
            break;
    }
    OPENSSL_cleanse(line, sizeof(line));
    OPENSSL_cleanse(&reader, sizeof(reader));
    session_finish(&args.server->sessions, args.client);
    return NULL;
}

static int fd_flags(int fd, bool nonblocking)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        return -1;
    if (nonblocking) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return -1;
    }
    return 0;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0700) == 0)
        return 0;
    if (errno != EEXIST)
        return -1;
    struct stat info;
    if (lstat(path, &info) != 0)
        return -1;
    if (!S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int listener_create(const struct sockaddr_in *address)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    if (fd_flags(fd, true) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(fd, (const struct sockaddr *)address, sizeof(*address)) != 0 ||
        listen(fd, (int)SESSION_LIMIT) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static void accept_client(int listener, server_state *server)
{
    struct sockaddr_in peer;
    socklen_t size = sizeof(peer);
    int fd = accept(listener, (struct sockaddr *)&peer, &size);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) {
            perror("accept");
            /* Avoid a busy loop if the process reaches its descriptor limit. */
            (void)poll(NULL, 0, 50);
        }
        return;
    }
    struct timeval timeout = {SOCKET_TIMEOUT_SECONDS, 0};
    char ip[INET_ADDRSTRLEN];
    if (fd_flags(fd, false) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
        close(fd);
        return;
    }
    session *client = session_add(&server->sessions, fd, ip);
    if (client == NULL) {
        static const char response[] = "ERR SERVER_BUSY\n";
        (void)send_all(fd, response, sizeof(response) - 1);
        close(fd);
        return;
    }
    worker_args *args = malloc(sizeof(*args));
    if (args == NULL) {
        static const char response[] = "ERR SERVER_BUSY\n";
        (void)send_all(fd, response, sizeof(response) - 1);
        session_remove(&server->sessions, client);
        return;
    }
    args->server = server;
    args->client = client;
    /* Workers inherit blocked shutdown signals; only the main thread handles
     * signals and accesses the self-pipe descriptor in signal context.
     */
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (error == 0) {
        error = pthread_create(&client->thread, NULL, client_worker, args);
        (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    if (error != 0) {
        free(args);
        static const char response[] = "ERR SERVER_BUSY\n";
        (void)send_all(fd, response, sizeof(response) - 1);
        session_remove(&server->sessions, client);
    }
}

static int serve(int listener, server_state *server)
{
    int notification[2];
    if (pipe(notification) != 0)
        return -1;
    int result = -1;
    if (fd_flags(notification[0], true) != 0 || fd_flags(notification[1], true) != 0)
        goto close_pipe;
    struct sigaction action = {0};
    action.sa_handler = on_shutdown;
    sigemptyset(&action.sa_mask);
    signal_fd = notification[1];
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0)
        goto close_pipe;

    struct pollfd descriptors[2] = {
        {listener, POLLIN, 0}, {notification[0], POLLIN, 0}
    };
    puts("READY");
    fflush(stdout);
    for (;;) {
        session_reap(&server->sessions);
        int ready = poll(descriptors, 2, 250);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (descriptors[1].revents != 0) {
            result = 0;
            break;
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            break;
        }
        if (descriptors[0].revents & POLLIN)
            accept_client(listener, server);
    }

close_pipe:
    signal_fd = -1;
    close(notification[0]);
    close(notification[1]);
    return result;
}

int main(int argc, char **argv)
{
    const char *ip = DEFAULT_SERVER_IP;
    const char *accounts = DEFAULT_ACCOUNTS;
    uint32_t port = DEFAULT_SERVER_PORT;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return 0;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            usage(stderr, argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--bind") == 0)
            ip = argv[i + 1];
        else if (strcmp(argv[i], "--port") == 0) {
            if (!decimal(argv[i + 1], UINT16_MAX, &port) || port == 0) {
                fputs("Invalid port\n", stderr);
                return 1;
            }
        } else if (strcmp(argv[i], "--accounts") == 0)
            accounts = argv[i + 1];
        else {
            usage(stderr, argv[0]);
            return 1;
        }
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        fputs("Invalid IPv4 bind address\n", stderr);
        return 1;
    }
    if (strcmp(accounts, DEFAULT_ACCOUNTS) == 0 &&
        (ensure_directory("data") != 0 || ensure_directory("data/server") != 0)) {
        perror("account directory");
        return 1;
    }
    server_state server;
    if (auth_init(&server.auth, accounts) != 0) {
        perror("account storage");
        return 1;
    }
    if (session_manager_init(&server.sessions) != 0) {
        perror("session manager");
        auth_destroy(&server.auth);
        return 1;
    }
    int listener = listener_create(&address);
    int result = -1;
    if (listener < 0)
        perror("listen");
    else {
        printf("Listening on %s:%u\n", ip, port);
        result = serve(listener, &server);
        if (result != 0)
            perror("server loop");
        close(listener);
    }
    session_shutdown(&server.sessions);
    session_manager_destroy(&server.sessions);
    auth_destroy(&server.auth);
    return result == 0 ? 0 : 1;
}
