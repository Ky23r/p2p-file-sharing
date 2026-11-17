#include "file_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

bool file_decimal(const char *text, uint64_t maximum, uint64_t *value)
{
    uint64_t result = 0;
    if (*text == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned digit = *p - '0';
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10))
            return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

bool file_hex_encode(const char *text, char *encoded, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(text);
    if (capacity == 0 || length > (capacity - 1) / 2)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)text[i];
        encoded[2 * i] = digits[byte >> 4];
        encoded[2 * i + 1] = digits[byte & 15];
    }
    encoded[2 * length] = '\0';
    return true;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9')
        return byte - '0';
    if (byte >= 'a' && byte <= 'f')
        return byte - 'a' + 10;
    return -1;
}

bool file_hex_decode(const char *encoded, char *text, size_t capacity)
{
    size_t length = strlen(encoded);
    if (length % 2 != 0 || capacity == 0 || length / 2 >= capacity)
        return false;
    for (size_t i = 0; i < length / 2; ++i) {
        int high = hex_digit((unsigned char)encoded[2 * i]);
        int low = hex_digit((unsigned char)encoded[2 * i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0))
            return false;
        text[i] = (char)(high * 16 + low);
    }
    text[length / 2] = '\0';
    return true;
}

bool file_name_valid(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= FILE_NAME_CAP || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)name[i];
        if (byte < 0x20 || byte == 0x7f || byte == '/' || byte == '\\')
            return false;
    }
    return true;
}

bool file_name_decode(const char *encoded, char name[FILE_NAME_CAP])
{
    return file_hex_decode(encoded, name, FILE_NAME_CAP) && file_name_valid(name);
}

bool file_tokens(char *line, size_t length, char **tokens, size_t expected)
{
    if (length == 0 || expected == 0 || line[0] == ' ' || line[length - 1] == ' ')
        return false;
    size_t count = 1;
    tokens[0] = line;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)line[i];
        if (byte < 0x20 || byte > 0x7e)
            return false;
        if (byte == ' ') {
            if (count == expected || line[i + 1] == ' ')
                return false;
            tokens[count++] = line + i + 1;
        }
    }
    if (count != expected)
        return false;
    for (size_t i = 1; i < count; ++i)
        tokens[i][-1] = '\0';
    return true;
}

bool file_error(const char *line, size_t length)
{
    return length > 4 && memcmp(line, "ERR ", 4) == 0 &&
           strspn(line + 4, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == length - 4;
}

int file_socket_timeout(int fd)
{
    const struct timeval timeout = {FILE_TIMEOUT_SECONDS, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

int file_connect(const char *ip, uint16_t port, ClientSignals *signals)
{
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (port == 0 || inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (client_signals_watch_socket(signals, fd) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || file_socket_timeout(fd) != 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        goto fail;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS)
            goto fail;
        struct pollfd descriptors[2] = {
            {fd, POLLOUT, 0}, {client_signals_fd(signals), POLLIN, 0}
        };
        int ready;
        do {
            ready = poll(descriptors, 2, FILE_TIMEOUT_SECONDS * 1000);
        } while (ready < 0 && errno == EINTR && client_signals_number(signals) == 0);
        if (client_signals_number(signals) != 0) {
            errno = ECANCELED;
            goto fail;
        }
        if (ready <= 0) {
            if (ready == 0)
                errno = ETIMEDOUT;
            goto fail;
        }
        int socket_error;
        socklen_t error_size = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0)
            goto fail;
        if (socket_error != 0) {
            errno = socket_error;
            goto fail;
        }
    }
    if (client_signals_number(signals) != 0) {
        errno = ECANCELED;
        goto fail;
    }
    if (fcntl(fd, F_SETFL, flags) == 0)
        return fd;
fail:;
    int error = errno;
    client_signals_close_socket(signals, fd);
    errno = error;
    return -1;
}
