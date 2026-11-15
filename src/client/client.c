#include "config.h"
#include "net_io.h"
#include "peer_server.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_CONFIG "config.txt"
#define DEFAULT_PEER_PORT 9001U
#define SOCKET_TIMEOUT_SECONDS 30

typedef struct {
    const char *ip;
    const char *config;
    uint16_t server_port;
    uint16_t peer_port;
} client_options;

enum request_result { REQUEST_FAILED = -1, REQUEST_REJECTED, REQUEST_OK };
enum input_result { INPUT_END, INPUT_ERROR, INPUT_INVALID, INPUT_OK };

/* The saved settings are available to the signal handler during password entry. */
static struct termios terminal_settings;
static volatile sig_atomic_t password_entry;

static void restore_terminal_on_signal(int number)
{
    if (password_entry) {
        tcflush(STDIN_FILENO, TCIFLUSH);
        tcsetattr(STDIN_FILENO, TCSANOW, &terminal_settings);
    }
    _exit(128 + number);
}

static int install_terminal_handlers(void)
{
    const int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE};
    struct sigaction action = {0};
    action.sa_handler = restore_terminal_on_signal;
    sigfillset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        if (sigaction(signals[i], &action, NULL) < 0)
            return -1;
    }
    return 0;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--server IPv4] [--port 1..65535]\n"
                    "       [--peer-port 1..65535] [--config PATH]\n"
                    "       %s --help\n"
                    "Defaults: server %s:%u, peer port %u, config %s\n",
            program, program, DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT,
            DEFAULT_PEER_PORT, DEFAULT_CONFIG);
}

static void commands(void)
{
    puts("register USER   Register an account\n"
         "login USER      Log in and announce the peer port\n"
         "hello           Announce the client ID and peer port again\n"
         "logout          Log out and keep the connection open\n"
         "help            Show commands\n"
         "quit            Close the connection and exit");
}

static bool parse_port(const char *text, uint16_t *port)
{
    if (*text == '\0' || text[strspn(text, "0123456789")] != '\0')
        return false;
    errno = 0;
    unsigned long value = strtoul(text, NULL, 10);
    if (errno == ERANGE || value == 0 || value > UINT16_MAX)
        return false;
    *port = (uint16_t)value;
    return true;
}

static bool parse_options(int argc, char **argv, client_options *options)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc)
            return false;
        const char *option = argv[i];
        const char *value = argv[i + 1];
        if (strcmp(option, "--server") == 0)
            options->ip = value;
        else if (strcmp(option, "--config") == 0)
            options->config = value;
        else if (strcmp(option, "--port") == 0) {
            if (!parse_port(value, &options->server_port))
                return false;
        } else if (strcmp(option, "--peer-port") == 0) {
            if (!parse_port(value, &options->peer_port))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static int connect_server(const struct sockaddr_in *address)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    const struct timeval timeout = {SOCKET_TIMEOUT_SECONDS, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0)
        return fd;

    int error = errno;
    close(fd);
    errno = error == EINPROGRESS ? ETIMEDOUT : error;
    return -1;
}

static enum request_result exchange(net_reader *server, const char *request,
                                    const char *expected)
{
    if (send_all(server->fd, request, strlen(request)) < 0) {
        perror("Sending request to server");
        return REQUEST_FAILED;
    }

    char response[PROTOCOL_MAX_LINE + 1];
    size_t length;
    int result = recv_line(server, response, sizeof(response), &length);
    if (result == NET_LINE_ERROR) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fputs("Timed out waiting for the server.\n", stderr);
        else
            perror("Reading response from server");
    } else if (result == NET_LINE_EOF) {
        fputs("Server closed the connection.\n", stderr);
    } else if (result == NET_LINE_OK && length == strlen(expected) &&
               memcmp(response, expected, length) == 0) {
        puts(response);
        return REQUEST_OK;
    } else if (result == NET_LINE_OK && length > 4 && memcmp(response, "ERR ", 4) == 0 &&
               strspn(response + 4, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == length - 4) {
        puts(response);
        if (strcmp(response, "ERR SERVER_BUSY") != 0 && strcmp(response, "ERR LINE_TOO_LONG") != 0)
            return REQUEST_REJECTED;
    } else {
        fputs("Invalid response from server.\n", stderr);
    }
    return REQUEST_FAILED;
}

static enum request_result announce_peer(net_reader *server, uint32_t id, uint16_t port)
{
    char request[sizeof("HELLO 4294967295 65535\n")];
    snprintf(request, sizeof(request), "HELLO %" PRIu32 " %u\n", id, (unsigned)port);
    return exchange(server, request, "OK HELLO");
}

/* Drain excess bytes before reading the next line. */
static enum input_result read_input(char *line, size_t capacity)
{
    size_t length = 0;
    bool invalid = false;
    int byte;
    while ((byte = getchar()) != '\n' && byte != EOF) {
        if (byte == '\0' || length == capacity - 1)
            invalid = true;
        else
            line[length++] = (char)byte;
    }
    if (byte == EOF && ferror(stdin))
        return INPUT_ERROR;
    if (byte == EOF && length == 0 && !invalid)
        return INPUT_END;
    if (byte == '\n' && length > 0 && line[length - 1] == '\r')
        --length;
    line[length] = '\0';
    return invalid ? INPUT_INVALID : INPUT_OK;
}

static bool valid_username(const char *username)
{
    if (username == NULL)
        return false;
    size_t length = strlen(username);
    return length >= USERNAME_MIN && length <= USERNAME_MAX &&
           strspn(username, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == length;
}

static bool valid_password(const char *password)
{
    size_t length = strlen(password);
    if (length < PASSWORD_MIN || length > PASSWORD_MAX)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)password[i];
        if (byte < 0x21 || byte > 0x7e)
            return false;
    }
    return true;
}

static enum input_result read_password(char *password, size_t capacity, bool interactive)
{
    if (!interactive)
        return read_input(password, capacity);

    if (tcgetattr(STDIN_FILENO, &terminal_settings) < 0)
        return INPUT_ERROR;
    struct termios hidden = terminal_settings;
    hidden.c_lflag &= ~(ECHO | ECHONL);
    password_entry = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) < 0) {
        password_entry = 0;
        return INPUT_ERROR;
    }

    fputs("Password: ", stdout);
    fflush(stdout);
    enum input_result result = read_input(password, capacity);
    int error = errno;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal_settings) < 0)
        return INPUT_ERROR;
    password_entry = 0;
    putchar('\n');
    errno = error;
    return result;
}

static enum request_result account_command(net_reader *server, bool registering,
                                           const char *username, bool interactive)
{
    bool username_valid = valid_username(username);
    char password[PASSWORD_MAX + 2];
    enum input_result input = INPUT_INVALID;
    /* Scripted account commands with a username carry a separate password line. */
    if (username_valid || (!interactive && username != NULL))
        input = read_password(password, sizeof(password), interactive);
    if (input != INPUT_OK || !username_valid || !valid_password(password)) {
        OPENSSL_cleanse(password, sizeof(password));
        if (input == INPUT_ERROR) {
            perror("Reading password");
            return REQUEST_FAILED;
        }
        if (input == INPUT_END) {
            fputs("Missing password.\n", stderr);
            return REQUEST_FAILED;
        }
        if (!username_valid)
            fprintf(stderr, "Username must be %u..%u letters, digits, or underscores.\n",
                    USERNAME_MIN, USERNAME_MAX);
        else
            fprintf(stderr, "Password must be %u..%u printable ASCII characters without spaces.\n",
                    PASSWORD_MIN, PASSWORD_MAX);
        return REQUEST_REJECTED;
    }

    char request[sizeof("REGISTER  \n") + USERNAME_MAX + PASSWORD_MAX];
    snprintf(request, sizeof(request), "%s %s %s\n", registering ? "REGISTER" : "LOGIN",
             username, password);
    OPENSSL_cleanse(password, sizeof(password));
    enum request_result result = exchange(server, request, registering ? "OK REGISTERED" : "OK LOGGED_IN");
    OPENSSL_cleanse(request, sizeof(request));
    return result;
}

static int run_client(net_reader *server, uint32_t id, uint16_t peer_port)
{
    bool interactive = isatty(STDIN_FILENO);
    if (interactive) {
        if (install_terminal_handlers() < 0) {
            perror("Preparing terminal");
            return EXIT_FAILURE;
        }
        printf("Connected with client ID %" PRIu32 ". Peer listener on port %u.\n"
               "Type 'help' for commands.\n", id, (unsigned)peer_port);
    }

    for (;;) {
        if (interactive) {
            fputs("p2p> ", stdout);
            fflush(stdout);
        }
        char line[PROTOCOL_MAX_LINE + 1];
        enum input_result input = read_input(line, sizeof(line));
        if (input == INPUT_END)
            return EXIT_SUCCESS;
        if (input == INPUT_ERROR) {
            perror("Reading command");
            return EXIT_FAILURE;
        }
        if (input == INPUT_INVALID) {
            fputs("Invalid input line.\n", stderr);
            continue;
        }
        if (*line == '\0')
            continue;

        char *argument = strchr(line, ' ');
        if (argument != NULL) {
            *argument = '\0';
            ++argument;
        }
        bool registering = strcmp(line, "register") == 0;
        enum request_result result = REQUEST_REJECTED;
        if (registering || strcmp(line, "login") == 0) {
            result = account_command(server, registering, argument, interactive);
            if (!registering && result == REQUEST_OK)
                result = announce_peer(server, id, peer_port);
        } else if (argument != NULL) {
            fputs("Unexpected argument. Type 'help' for commands.\n", stderr);
        } else if (strcmp(line, "hello") == 0) {
            result = announce_peer(server, id, peer_port);
        } else if (strcmp(line, "logout") == 0) {
            result = exchange(server, "LOGOUT\n", "OK LOGGED_OUT");
        } else if (strcmp(line, "help") == 0) {
            commands();
        } else if (strcmp(line, "quit") == 0) {
            return EXIT_SUCCESS;
        } else {
            fputs("Unknown command. Type 'help' for commands.\n", stderr);
        }
        if (result == REQUEST_FAILED)
            return EXIT_FAILURE;
    }
}

int main(int argc, char **argv)
{
    client_options options = {DEFAULT_SERVER_IP, DEFAULT_CONFIG, DEFAULT_SERVER_PORT, DEFAULT_PEER_PORT};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        commands();
        return EXIT_SUCCESS;
    }
    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.server_port);
    if (inet_pton(AF_INET, options.ip, &address.sin_addr) != 1) {
        fputs("Invalid server IPv4 address.\n", stderr);
        return EXIT_FAILURE;
    }

    uint32_t id;
    if (load_client_id(options.config, &id) != 0) {
        fprintf(stderr, "Client ID file %s: %s\n", options.config, strerror(errno));
        return EXIT_FAILURE;
    }
    int peer_listener = open_peer_listener(options.peer_port);
    if (peer_listener < 0) {
        fprintf(stderr, "Starting peer listener on port %u: %s\n",
                (unsigned)options.peer_port, strerror(errno));
        return EXIT_FAILURE;
    }

    int fd = connect_server(&address);
    if (fd < 0) {
        fprintf(stderr, "Connecting to %s:%u: %s\n", options.ip,
                (unsigned)options.server_port, strerror(errno));
        close(peer_listener);
        return EXIT_FAILURE;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    net_reader server;
    net_reader_init(&server, fd);
    int result = run_client(&server, id, options.peer_port);
    close(fd);
    close(peer_listener);
    return result;
}
