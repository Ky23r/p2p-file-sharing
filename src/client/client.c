#include "config.h"
#include "client_session.h"
#include "peer_server.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_CONFIG "config.txt"
#define DEFAULT_PEER_PORT 9001U

typedef struct {
    const char *ip;
    const char *config;
    const char *index;
    const char *downloads;
    uint16_t server_port;
    uint16_t peer_port;
} client_options;

enum input_result { INPUT_END, INPUT_EMPTY, INPUT_ERROR, INPUT_INVALID, INPUT_OK };

typedef struct {
    int control_failure_fd;
    ClientSignals *signals;
} InputContext;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--server IPv4] [--port 1..65535]\n"
                    "       [--peer-port 1..65535] [--config PATH]\n"
                    "       [--index PATH] [--downloads DIRECTORY]\n"
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
         "share PATH      Save and announce a local file (spaces allowed)\n"
         "unshare NAME    Stop sharing a filename\n"
         "shares          List local shares and announcement state\n"
         "search NAME     Find available peers by exact filename\n"
         "download NUMBER Download a result from the latest search\n"
         "help            Show commands\n"
         "quit            Close the connection and exit");
}

static bool parse_port(const char *text, uint16_t *port)
{
    uint64_t value;
    if (!protocol_parse_uint64(text, UINT16_MAX, &value) || value == 0)
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
        else if (strcmp(option, "--index") == 0)
            options->index = value;
        else if (strcmp(option, "--downloads") == 0)
            options->downloads = value;
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

static enum input_result read_input(char **line, size_t *capacity, const InputContext *input)
{
    size_t length = 0;
    bool invalid = false;
    int byte;
    for (;;) {
        struct pollfd descriptors[3] = {
            {STDIN_FILENO, POLLIN, 0}, {input->control_failure_fd, POLLIN, 0},
            {client_signals_fd(input->signals), POLLIN, 0}
        };
        int ready;
        do {
            ready = poll(descriptors, 3, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            return INPUT_ERROR;
        if (descriptors[2].revents != 0) {
            errno = ECANCELED;
            return INPUT_ERROR;
        }
        if (descriptors[1].revents != 0) {
            errno = ECONNRESET;
            return INPUT_ERROR;
        }
        if (descriptors[0].revents & POLLNVAL) {
            errno = EBADF;
            return INPUT_ERROR;
        }
        byte = getchar();
        if (byte == '\n' || byte == EOF)
            break;
        if (byte == '\0')
            invalid = true;
        if (invalid)
            continue;
        if (*capacity == 0 || length == *capacity - 1) {
            if (*capacity > SIZE_MAX / 2) {
                errno = EOVERFLOW;
                return INPUT_ERROR;
            }
            size_t grown = *capacity == 0 ? 128 : *capacity * 2;
            char *buffer = malloc(grown);
            if (buffer == NULL)
                return INPUT_ERROR;
            if (*line != NULL) {
                memcpy(buffer, *line, length);
                OPENSSL_cleanse(*line, length);
                free(*line);
            }
            *line = buffer;
            *capacity = grown;
        }
        (*line)[length++] = (char)byte;
    }
    if (byte == EOF && ferror(stdin))
        return INPUT_ERROR;
    if (invalid)
        return INPUT_INVALID;
    if (length == 0)
        return byte == EOF ? INPUT_END : INPUT_EMPTY;
    if (byte == '\n' && (*line)[length - 1] == '\r')
        --length;
    (*line)[length] = '\0';
    return INPUT_OK;
}

static enum input_result read_password(char **password, size_t *capacity, bool interactive,
                                       const InputContext *input)
{
    if (!interactive)
        return read_input(password, capacity, input);

    struct termios terminal_settings;
    if (tcgetattr(STDIN_FILENO, &terminal_settings) < 0)
        return INPUT_ERROR;
    struct termios hidden = terminal_settings;
    hidden.c_lflag &= ~(ECHO | ECHONL);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) < 0)
        return INPUT_ERROR;

    fputs("Password: ", stdout);
    fflush(stdout);
    enum input_result result = read_input(password, capacity, input);
    int error = errno;
    /* Discard a partly entered password before restoring terminal echo. */
    if (client_signals_number(input->signals) != 0)
        tcflush(STDIN_FILENO, TCIFLUSH);
    int restored;
    do {
        restored = tcsetattr(STDIN_FILENO, TCSANOW, &terminal_settings);
    } while (restored < 0 && errno == EINTR);
    if (restored < 0)
        return INPUT_ERROR;
    putchar('\n');
    errno = error;
    return result;
}

static enum request_result account_command(ClientSession *server, bool registering,
                                           const char *username, bool interactive, const InputContext *context)
{
    bool username_valid = protocol_valid_username(username);
    char *password = NULL;
    size_t capacity = 0;
    enum input_result input = INPUT_INVALID;
    /* Scripted account commands with a username carry a separate password line. */
    if (username_valid || (!interactive && username != NULL))
        input = read_password(&password, &capacity, interactive, context);
    if (input != INPUT_OK || !username_valid || !protocol_valid_password(password)) {
        if (password != NULL) {
            OPENSSL_cleanse(password, capacity);
            free(password);
        }
        if (input == INPUT_ERROR) {
            if (client_signals_number(context->signals) == 0)
                perror("Reading password");
            return REQUEST_FAILED;
        }
        if (input == INPUT_END) {
            fputs("Missing password.\n", stderr);
            return REQUEST_FAILED;
        }
        if (!username_valid)
            fputs("Username must contain letters, digits, or underscores.\n", stderr);
        else
            fprintf(stderr, "Password must contain at least %u printable ASCII characters without spaces.\n",
                    PASSWORD_MIN);
        return REQUEST_REJECTED;
    }

    const char *command = registering ? "REGISTER" : "LOGIN";
    size_t length = strlen(command) + strlen(username) + strlen(password) + sizeof("  " CRLF);
    char *request = malloc(length);
    if (request != NULL)
        snprintf(request, length, "%s %s %s" CRLF, command, username, password);
    OPENSSL_cleanse(password, capacity);
    free(password);
    if (request == NULL) {
        perror("Preparing account request");
        return REQUEST_FAILED;
    }
    enum request_result result = client_exchange(server, request, registering ? STATUS_CREATED : STATUS_OK);
    OPENSSL_cleanse(request, length);
    free(request);
    return result;
}

static int run_client(ClientSession *server, uint32_t id, uint16_t peer_port, ClientSignals *signals)
{
    bool interactive = isatty(STDIN_FILENO);
    InputContext input_context = {client_session_failure_fd(server), signals};
    if (interactive) {
        printf("Connected with client ID %" PRIu32 ". Peer listener on port %u.\n"
               "Type 'help' for commands.\n", id, (unsigned)peer_port);
    }

    char *line = NULL;
    size_t capacity = 0;
    int exit_status = EXIT_FAILURE;
    for (;;) {
        if (interactive) {
            fputs("p2p> ", stdout);
            fflush(stdout);
        }
        enum input_result input = read_input(&line, &capacity, &input_context);
        if (input == INPUT_END) {
            exit_status = client_session_failed(server) ? EXIT_FAILURE : EXIT_SUCCESS;
            break;
        }
        if (input == INPUT_ERROR) {
            if (client_signals_number(signals) == 0)
                perror("Reading command");
            break;
        }
        if (input == INPUT_INVALID) {
            fputs("Invalid input line.\n", stderr);
            continue;
        }
        if (input == INPUT_EMPTY || *line == '\0')
            continue;
        if (client_session_failed(server))
            break;

        char *argument = strchr(line, ' ');
        if (argument != NULL) {
            *argument = '\0';
            ++argument;
        }
        bool registering = strcmp(line, "register") == 0;
        enum request_result result = REQUEST_REJECTED;
        if (registering || strcmp(line, "login") == 0) {
            result = account_command(server, registering, argument, interactive, &input_context);
            if (!registering && result == REQUEST_OK)
                result = client_hello(server);
        } else if (strcmp(line, "share") == 0) {
            result = client_share(server, argument);
        } else if (strcmp(line, "unshare") == 0) {
            result = client_unshare(server, argument);
        } else if (strcmp(line, "search") == 0) {
            result = client_search(server, argument);
        } else if (strcmp(line, "download") == 0) {
            result = client_download(server, argument);
        } else if (argument != NULL) {
            fputs("Unexpected argument. Type 'help' for commands.\n", stderr);
        } else if (strcmp(line, "hello") == 0) {
            result = client_hello(server);
        } else if (strcmp(line, "logout") == 0) {
            result = client_logout(server);
        } else if (strcmp(line, "shares") == 0) {
            client_shares(server);
        } else if (strcmp(line, "help") == 0) {
            commands();
        } else if (strcmp(line, "quit") == 0) {
            exit_status = EXIT_SUCCESS;
            break;
        } else {
            fputs("Unknown command. Type 'help' for commands.\n", stderr);
        }
        if (result == REQUEST_FAILED)
            break;
    }
    free(line);
    return exit_status;
}

int main(int argc, char **argv)
{
    client_options options = {DEFAULT_SERVER_IP, DEFAULT_CONFIG, "index.txt", ".",
                              DEFAULT_SERVER_PORT, DEFAULT_PEER_PORT};
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

    ClientSignals *signals = client_signals_start();
    if (signals == NULL) {
        perror("Preparing client signals");
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    int peer_listener = -1, fd = -1;
    SharedIndex *index = NULL;
    PeerServer *peers = NULL;
    ClientSession *server = NULL;
    uint32_t id;
    if (load_client_id(options.config, &id) != 0) {
        fprintf(stderr, "Client ID file %s: %s\n", options.config, strerror(errno));
        goto cleanup;
    }
    peer_listener = open_peer_listener(options.peer_port);
    if (peer_listener < 0) {
        fprintf(stderr, "Starting peer listener on port %u: %s\n",
                (unsigned)options.peer_port, strerror(errno));
        goto cleanup;
    }

    index = shared_index_load(options.index);
    if (index == NULL) {
        fprintf(stderr, "Shared-file index %s: %s\n", options.index, strerror(errno));
        goto cleanup;
    }
    peers = peer_server_start(peer_listener, index);
    if (peers == NULL) {
        perror("Starting peer workers");
        goto cleanup;
    }
    fd = file_connect(options.ip, options.server_port, signals);
    if (fd < 0) {
        fprintf(stderr, "Connecting to %s:%u: %s\n", options.ip,
                (unsigned)options.server_port, strerror(errno));
        goto cleanup;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Input polling must see every byte, including redirected multi-line input. */
    setvbuf(stdin, NULL, _IONBF, 0);
    server = client_session_start(fd, id, options.peer_port, index, options.downloads, signals);
    if (server == NULL)
        perror("Starting control session");
    else
        result = run_client(server, id, options.peer_port, signals);
cleanup:
    client_session_stop(server);
    client_signals_close_socket(signals, fd);
    peer_server_stop(peers);
    shared_index_destroy(index);
    if (peer_listener >= 0)
        close(peer_listener);
    int number = client_signals_stop(signals);
    if (number != 0)
        return 128 + number;
    return result;
}
