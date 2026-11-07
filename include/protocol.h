#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

/* Wire protocol: ASCII, case-sensitive commands, single ASCII spaces, LF only.
 * At most 4096 bytes before LF; no CR, NUL, tabs, or other control bytes.
 * REGISTER <username> <password> -> OK REGISTERED (does not log in)
 * LOGIN <username> <password>    -> OK LOGGED_IN
 * HELLO <client_id> <port>       -> OK HELLO (login required)
 * LOGOUT                         -> OK LOGGED_OUT (login required)
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
#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 9000U

#endif
