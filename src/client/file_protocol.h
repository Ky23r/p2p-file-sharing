#ifndef P2P_FILE_PROTOCOL_H
#define P2P_FILE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILE_NAME_CAP 256U
#define FILE_NAME_HEX_CAP (2U * FILE_NAME_CAP)
#define FILE_PATH_CAP 4096U
#define FILE_ENTRY_LIMIT 256U
#define FILE_SIZE_MAX UINT64_C(1099511627776)
#define FILE_TIMEOUT_SECONDS 30

typedef struct {
    uint32_t client_id;
    char ip[16];
    uint16_t port;
    uint64_t size;
    char name[FILE_NAME_CAP];
} file_result;

bool file_decimal(const char *text, uint64_t maximum, uint64_t *value);
bool file_hex_encode(const char *text, char *encoded, size_t capacity);
bool file_hex_decode(const char *encoded, char *text, size_t capacity);
bool file_name_valid(const char *name);
bool file_name_decode(const char *encoded, char name[FILE_NAME_CAP]);
/* Strict printable ASCII, single spaces; replaces separators with NUL. */
bool file_tokens(char *line, size_t length, char **tokens, size_t expected);
bool file_error(const char *line, size_t length);
int file_connect(const char *ip, uint16_t port);
int file_socket_timeout(int fd);

#endif
