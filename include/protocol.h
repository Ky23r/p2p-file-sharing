#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

/* See docs/PROTOCOL.md for the shared control and peer protocols. */
#define CRLF "\r\n"
enum protocol_status {
    STATUS_OK = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_UNAUTHORIZED = 401,
    STATUS_FORBIDDEN = 403,
    STATUS_NOT_FOUND = 404,
    STATUS_CONFLICT = 409,
    STATUS_INTERNAL_ERROR = 500,
    STATUS_UNAVAILABLE = 503
};

#define CMD_REGISTER "REGISTER"
#define CMD_LOGIN "LOGIN"
#define CMD_HELLO "HELLO"
#define CMD_LOGOUT "LOGOUT"
#define CMD_SHARE "SHARE"
#define CMD_UNSHARE "UNSHARE"
#define CMD_SEARCH "SEARCH"
#define CMD_FAIL "FAIL"
#define CMD_GET "GET"

#define PASSWORD_MIN 8U
#define CONTROL_LINE_MAX 65536U /* Bytes before LF, including CR. */
#define PROTOCOL_FILENAME_MAX 255U
#define PROTOCOL_FILENAME_TOKEN_MAX (PROTOCOL_FILENAME_MAX * 3U)
#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 9000U

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Split printable ASCII with single spaces. Return 0 on invalid input. */
size_t protocol_tokens(char *line, size_t length, char **tokens, size_t capacity);
/* Return a canonical verb for a printable request, regardless of verb case. */
const char *protocol_command(const char *line, size_t length);
/* Decode a bare three-digit status, or return zero on invalid syntax. */
int protocol_status(const char *line, size_t length);
bool protocol_valid_username(const char *username);
bool protocol_valid_password(const char *password);

/* Decimal without a sign; leading zeros are accepted. */
bool protocol_parse_uint64(const char *text, uint64_t maximum, uint64_t *value);
/* Basenames may include non-ASCII bytes. Control bytes, slash, backslash,
 * "." and ".." are invalid. Unreserved ASCII stays literal; other bytes use uppercase %HH.
 * Capacities include NUL. Failure leaves output unspecified.
 */
bool protocol_valid_filename(const char *name);
bool protocol_encode_filename(const char *name, char *token, size_t capacity);
bool protocol_decode_filename(const char *token, char *name, size_t capacity);

#endif
