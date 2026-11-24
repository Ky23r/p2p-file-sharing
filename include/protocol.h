#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

/* See docs/PROTOCOL.md for the shared control and peer protocols. */
#define CRLF "\r\n"
#define STATUS_OK "200"
#define STATUS_SERVICE_UNAVAILABLE "421"
#define REPLY_OK STATUS_OK " OK"
#define REPLY_CONTINUATION STATUS_OK "-"
#define REPLY_SERVICE_UNAVAILABLE STATUS_SERVICE_UNAVAILABLE " Service unavailable"
#define REPLY_TEMPORARY_FILE_ERROR "450 File temporarily unavailable"
#define REPLY_PROCESSING_ERROR "451 Local processing error"
#define REPLY_INVALID_COMMAND "500 Invalid or unknown command"
#define REPLY_INVALID_ARGUMENTS "501 Invalid arguments"
#define REPLY_BAD_SEQUENCE "503 Bad command sequence"
#define REPLY_LOGIN_REQUIRED "530 Not logged in"
#define REPLY_BAD_CREDENTIALS "530 Invalid credentials"
#define REPLY_ACCOUNT_EXISTS "550 Account already exists"
#define REPLY_CLIENT_ID_IN_USE "550 Client ID in use"
#define REPLY_CLIENT_ID_MISMATCH "550 Client ID does not match session"
#define REPLY_FILE_NOT_SHARED "550 File not shared"
#define REPLY_FILE_UNAVAILABLE "550 File unavailable"
#define REPLY_FILE_SIZE_CHANGED "550 File size changed"

#define CMD_REGISTER "REGISTER"
#define CMD_LOGIN "LOGIN"
#define CMD_HELLO "HELLO"
#define CMD_LOGOUT "LOGOUT"
#define CMD_SHARE "SHARE"
#define CMD_UNSHARE "UNSHARE"
#define CMD_SEARCH "SEARCH"
#define CMD_GET "GET"

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
/* Return a canonical verb for a printable request, regardless of verb case. */
const char *protocol_command(const char *line, size_t length);
/* Match a final reply by its three-digit code, independent of the reply text. */
bool protocol_reply_is(const char *line, size_t length, const char *code);
/* A single-line 4xx or 5xx reply with nonempty printable text. */
bool protocol_error(const char *line, size_t length);
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
