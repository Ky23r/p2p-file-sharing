#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

/* Wire protocol: ASCII, case-sensitive commands, single ASCII spaces, LF only.
 * At most 4096 bytes before LF; no CR, NUL, tabs, or other control bytes.
 * REGISTER <username> <password> -> OK REGISTERED (does not log in)
 * LOGIN <username> <password>    -> OK LOGGED_IN
 * HELLO <client_id> <port>       -> OK HELLO (login required)
 * LOGOUT                         -> OK LOGGED_OUT (login required)
 * SHARE <filename> <size>       -> OK SHARED
 * UNSHARE <filename>            -> OK UNSHARED
 * SEARCH <filename>             -> OK RESULTS <count>, then count FILE lines
 * REPORT <client_id> <filename> <reason> -> OK REPORTED
 * File commands require login and HELLO. Filenames use canonical percent
 * encoding; sizes are uint64 decimal. See docs/PROTOCOL.md for the contract.
 * Usernames: 3..32 ASCII letters, digits, or underscore (case-sensitive).
 * Passwords: 8..128 printable ASCII bytes, excluding space (0x21..0x7e).
 * Decimal client IDs: 0..4294967295; ports: 1..65535. No signs.
 * Errors: ERR <ERROR_CODE>\n. See README.md for codes and state transitions.
 * Registration/login allowed only while logged out. LOGOUT keeps TCP open.
 * EOF discards incomplete requests. Oversized requests close the connection.
 * Authentication over this plaintext transport is for controlled local use.
 */
#define PROTOCOL_MAX_LINE 4096U
#define USERNAME_MIN 3U
#define USERNAME_MAX 32U
#define PASSWORD_MIN 8U
#define PASSWORD_MAX 128U
#define PROTOCOL_FILENAME_MAX 255U
#define PROTOCOL_FILENAME_TOKEN_MAX (PROTOCOL_FILENAME_MAX * 3U)
#define PROTOCOL_SHARE_LIMIT 128U
#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 9000U

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decimal without a sign; leading zeros are accepted. */
bool protocol_parse_uint64(const char *text, uint64_t maximum, uint64_t *value);
/* Printable ASCII basenames, excluding slash, backslash, "." and "..".
 * Unreserved ASCII stays literal; other bytes use uppercase %HH.
 * Capacities include NUL. Failure leaves output unspecified.
 */
bool protocol_encode_filename(const char *name, char *token, size_t capacity);
bool protocol_decode_filename(const char *token, char *name, size_t capacity);

#endif
