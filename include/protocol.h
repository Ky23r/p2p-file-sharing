#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

/* See docs/PROTOCOL.md for the shared control and peer protocols. */
#define CRLF "\r\n"
#define STATUS_OK "200"
#define STATUS_CREATED "201"
#define STATUS_SEARCH_END STATUS_OK " END"
#define STATUS_BAD_REQUEST "400"
#define STATUS_UNAUTHORIZED "401"
#define STATUS_FORBIDDEN "403"
#define STATUS_NOT_FOUND "404"
#define STATUS_CONFLICT "409"
#define STATUS_ERROR "500"
#define STATUS_UNKNOWN_COMMAND "501"
#define STATUS_BUSY "503"

#define PASSWORD_MIN 8U
#define PROTOCOL_FILENAME_MAX 255U
#define PROTOCOL_FILENAME_TOKEN_MAX (PROTOCOL_FILENAME_MAX * 3U)
#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 9000U

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Split printable ASCII with single spaces. Return 0 on invalid input. */
size_t protocol_tokens(char *line, size_t length, char **tokens, size_t capacity);
bool protocol_error(const char *line, size_t length);
bool protocol_valid_username(const char *username);
bool protocol_valid_password(const char *password);

/* Decimal without a sign; leading zeros are accepted. */
bool protocol_parse_uint64(const char *text, uint64_t maximum, uint64_t *value);
/* Printable ASCII basenames, excluding slash, backslash, "." and "..".
 * Unreserved ASCII stays literal; other bytes use uppercase %HH.
 * Capacities include NUL. Failure leaves output unspecified.
 */
bool protocol_encode_filename(const char *name, char *token, size_t capacity);
bool protocol_decode_filename(const char *token, char *name, size_t capacity);

#endif
