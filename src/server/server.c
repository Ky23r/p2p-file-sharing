#include "auth.h"
#include "net_io.h"
#include "protocol.h"
#include "service_log.h"
#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
    service_log log;
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
    fprintf(stream, "Usage: %s [--bind IPv4] [--port 1..65535] [--accounts PATH] [--log PATH]\n"
            "       %s --help\n"
            "Defaults: %s:%u, accounts: %s\n"
            "Default log: <accounts-path>.log\n"
            "Active socket I/O timeout: %d seconds. Idle connections use TCP keepalive.\n"
            "SIGINT/SIGTERM gracefully stop the server.\n"
            "Plaintext authentication is for controlled local testing only.\n",
            program, program, DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT,
            DEFAULT_ACCOUNTS, SOCKET_TIMEOUT_SECONDS);
}

static bool decimal(const char *text, uint32_t maximum, uint32_t *value)
{
    uint64_t result;
    if (!protocol_parse_uint64(text, maximum, &result))
        return false;
    *value = (uint32_t)result;
    return true;
}

static int auth_response(enum auth_result result)
{
    switch (result) {
    case AUTH_OK: return STATUS_OK;
    case AUTH_INVALID: return STATUS_BAD_REQUEST;
    case AUTH_USER_EXISTS: return STATUS_CONFLICT;
    case AUTH_BAD_CREDENTIALS: return STATUS_UNAUTHORIZED;
    case AUTH_ERROR: return STATUS_INTERNAL_ERROR;
    }
    return STATUS_INTERNAL_ERROR;
}

static int session_response(enum session_result result)
{
    switch (result) {
    case SESSION_OK: return STATUS_OK;
    case SESSION_UNAUTHORIZED: return STATUS_UNAUTHORIZED;
    case SESSION_ID_IN_USE: return STATUS_CONFLICT;
    case SESSION_WRONG_CLIENT:
    case SESSION_HELLO_REQUIRED: return STATUS_FORBIDDEN;
    default: return STATUS_INTERNAL_ERROR;
    }
}

static int dispatch(server_state *server, session *client, const char *command,
                    char *line, size_t length, file_match **matches, size_t *match_count,
                    const char **username, char *detail, size_t detail_capacity)
{
    char *args[4];
    size_t count = protocol_tokens(line, length, args, sizeof(args) / sizeof(args[0]));
    if (count == 0)
        return STATUS_BAD_REQUEST;

    if (strcmp(command, CMD_REGISTER) == 0 || strcmp(command, CMD_LOGIN) == 0) {
        if (count != 3 || !protocol_valid_username(args[1]) || !protocol_valid_password(args[2]))
            return STATUS_BAD_REQUEST;
        if (session_authenticated(&server->sessions, client))
            return STATUS_CONFLICT;
        bool registering = strcmp(command, CMD_REGISTER) == 0;
        enum auth_result result = registering ? auth_register(&server->auth, args[1], args[2]) :
                                               auth_login(&server->auth, args[1], args[2]);
        if (result == AUTH_OK) {
            if (!registering) {
                enum session_result login = session_login(&server->sessions, client);
                if (login != SESSION_OK)
                    return session_response(login);
            }
            *username = args[1];
        }
        return auth_response(result);
    }
    if (strcmp(command, CMD_HELLO) == 0) {
        uint32_t id, port;
        if (count != 3 || !decimal(args[1], UINT32_MAX, &id) ||
            !decimal(args[2], UINT16_MAX, &port) || port == 0)
            return STATUS_BAD_REQUEST;
        enum session_result result = session_hello(&server->sessions, client, id, (uint16_t)port);
        if (result == SESSION_OK)
            snprintf(detail, detail_capacity, "client_id=%" PRIu32 " port=%u", id, (unsigned)port);
        return session_response(result);
    }
    if (strcmp(command, CMD_LOGOUT) == 0) {
        if (count != 1)
            return STATUS_BAD_REQUEST;
        return session_logout(&server->sessions, client) ? STATUS_OK : STATUS_UNAUTHORIZED;
    }
    char decoded[PROTOCOL_FILENAME_MAX + 1];
    if (strcmp(command, CMD_SHARE) == 0) {
        uint32_t id;
        if (count != 3 || !decimal(args[1], UINT32_MAX, &id) ||
            !protocol_decode_filename(args[2], decoded, sizeof(decoded)))
            return STATUS_BAD_REQUEST;
        enum session_result result = session_share(&server->sessions, client, id, args[2]);
        if (result == SESSION_OK)
            snprintf(detail, detail_capacity, "client_id=%" PRIu32 " filename=%s", id, args[2]);
        return session_response(result);
    }
    if (strcmp(command, CMD_FAIL) == 0) {
        uint32_t id;
        if (count != 4 || !decimal(args[1], UINT32_MAX, &id) ||
            !protocol_decode_filename(args[2], decoded, sizeof(decoded)))
            return STATUS_BAD_REQUEST;
        int code = protocol_status(args[3], strlen(args[3]));
        if (code != STATUS_NOT_FOUND && code != STATUS_UNAVAILABLE)
            return STATUS_BAD_REQUEST;
        enum session_result result = session_download_failure(&server->sessions, client, id, args[2],
                                                              code == STATUS_NOT_FOUND);
        if (result == SESSION_OK)
            snprintf(detail, detail_capacity, "client_id=%" PRIu32 " filename=%s code=%s", id, args[2], args[3]);
        return session_response(result);
    }
    if (strcmp(command, CMD_UNSHARE) == 0 || strcmp(command, CMD_SEARCH) == 0) {
        if (count != 2 || !protocol_decode_filename(args[1], decoded, sizeof(decoded)))
            return STATUS_BAD_REQUEST;
        bool searching = strcmp(command, CMD_SEARCH) == 0;
        enum session_result result = searching ?
            session_search(&server->sessions, client, args[1], matches, match_count) :
            session_unshare(&server->sessions, client, args[1]);
        if (result == SESSION_OK)
            snprintf(detail, detail_capacity, "filename=%s", args[1]);
        return session_response(result);
    }
    return STATUS_BAD_REQUEST;
}

static void *client_worker(void *argument)
{
    worker_args args = *(worker_args *)argument;
    free(argument);
    net_reader reader;
    char *line = NULL;
    size_t capacity = 0, length;
    int fd = args.client->fd;
    net_reader_init(&reader, fd);
    service_log_write(&args.server->log, "CONNECT fd=%d ip=%s", fd, args.client->ip);
    struct pollfd incoming = {fd, POLLIN, 0};
    for (;;) {
        /* Wait for the first byte without the receive timeout. Once a request
         * starts, recv_line bounds waits for the rest of it. */
        if (reader.next == reader.used) {
            int ready;
            do {
                ready = poll(&incoming, 1, -1);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0)
                break;
        }
        int result = recv_line_bounded(&reader, &line, &capacity, &length, CONTROL_LINE_MAX);
        if (result == NET_LINE_TOO_LONG) {
            (void)send_status(fd, STATUS_BAD_REQUEST);
            break;
        }
        if (result != NET_LINE_OK && result != NET_LINE_INVALID)
            break;

        const char *command = result == NET_LINE_OK ? protocol_command(line, length) : NULL;
        const char *username = NULL;
        int status = STATUS_BAD_REQUEST;
        file_match *matches = NULL;
        size_t match_count = 0;
        char detail[PROTOCOL_FILENAME_TOKEN_MAX + 64] = "";
        if (command != NULL && strcmp(command, CMD_GET) != 0) {
            status = dispatch(args.server, args.client, command, line, length, &matches, &match_count,
                              &username, detail, sizeof(detail));
        }
        service_log_write(&args.server->log, "REQUEST fd=%d ip=%s command=%s result=%d%s%s%s%s",
                          fd, args.client->ip, command != NULL ? command : "UNKNOWN", status,
                          username != NULL ? " username=" : "", username != NULL ? username : "",
                          *detail != '\0' ? " " : "", detail);

        OPENSSL_cleanse(line, capacity);
        OPENSSL_cleanse(reader.buffer, reader.next);
        bool sent;
        if (status == STATUS_OK && strcmp(command, CMD_SEARCH) == 0) {
            char record[64];
            int length = snprintf(record, sizeof(record), "%d %zu" CRLF, status, match_count);
            sent = send_all(fd, record, (size_t)length) >= 0;
            for (size_t i = 0; sent && i < match_count; ++i) {
                length = snprintf(record, sizeof(record), "%" PRIu32 " %s %u" CRLF,
                                  matches[i].client_id, matches[i].ip, (unsigned)matches[i].port);
                sent = send_all(fd, record, (size_t)length) >= 0;
            }
        } else {
            sent = send_status(fd, status) == 0;
        }
        free(matches);
        if (!sent)
            break;
    }
    if (line != NULL) {
        OPENSSL_cleanse(line, capacity);
        free(line);
    }
    OPENSSL_cleanse(&reader, sizeof(reader));
    service_log_write(&args.server->log, "DISCONNECT fd=%d ip=%s", args.client->fd, args.client->ip);
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
        listen(fd, SOMAXCONN) != 0) {
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
        net_enable_keepalive(fd) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
        close(fd);
        return;
    }
    session *client = session_add(&server->sessions, fd, ip);
    if (client == NULL) {
        (void)send_status(fd, STATUS_UNAVAILABLE);
        close(fd);
        return;
    }
    worker_args *args = malloc(sizeof(*args));
    if (args == NULL) {
        (void)send_status(fd, STATUS_UNAVAILABLE);
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
        (void)send_status(fd, STATUS_UNAVAILABLE);
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
    const char *log_path = NULL;
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
        else if (strcmp(argv[i], "--log") == 0)
            log_path = argv[i + 1];
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
    char *default_log = NULL;
    if (log_path == NULL) {
        default_log = malloc(strlen(accounts) + sizeof(".log"));
        if (default_log != NULL) {
            sprintf(default_log, "%s.log", accounts);
            log_path = default_log;
        }
    }
    if (log_path == NULL || service_log_init(&server.log, log_path) != 0) {
        perror("service log");
        free(default_log);
        session_manager_destroy(&server.sessions);
        auth_destroy(&server.auth);
        return 1;
    }
    free(default_log);
    struct stat accounts_info, log_info;
    if (fstat(server.auth.fd, &accounts_info) != 0 || fstat(server.log.fd, &log_info) != 0 ||
        (accounts_info.st_dev == log_info.st_dev && accounts_info.st_ino == log_info.st_ino)) {
        fprintf(stderr, "service log must be separate from account storage\n");
        service_log_destroy(&server.log);
        session_manager_destroy(&server.sessions);
        auth_destroy(&server.auth);
        return 1;
    }
    service_log_write(&server.log, "START bind=%s port=%u", ip, port);
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
    service_log_write(&server.log, "STOP");
    service_log_destroy(&server.log);
    session_manager_destroy(&server.sessions);
    auth_destroy(&server.auth);
    return result == 0 ? 0 : 1;
}
