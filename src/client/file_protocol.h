#ifndef P2P_FILE_PROTOCOL_H
#define P2P_FILE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "client_signals.h"

#define FILE_NAME_CAP 256U
#define FILE_NAME_HEX_CAP (2U * FILE_NAME_CAP)
#define FILE_PATH_CAP 4096U
#define FILE_ENTRY_LIMIT 256U
#define FILE_SIZE_MAX UINT64_C(1099511627776)
#define FILE_TIMEOUT_SECONDS 30
#define FILE_TRANSFER_BUFFER_SIZE 65536U

typedef struct {
    uint32_t client_id;
    char ip[16];
    uint16_t port;
    uint64_t size;
    char name[FILE_NAME_CAP];
} FileSearchResult;

/* Parsing helpers borrow NUL-terminated strings and caller-provided buffers;
 * pointers must be non-NULL. Outputs are usable only when true is returned.
 */

/* Accept unsigned decimal through maximum, rejecting signs and overflow. */
bool file_decimal(const char *text, uint64_t maximum, uint64_t *value);
/* Encode bytes as lowercase hex; capacity includes the trailing NUL. */
bool file_hex_encode(const char *text, char *encoded, size_t capacity);
/* Decode lowercase hex, rejecting malformed input and decoded NULs. capacity
 * includes NUL; failure may leave a partly written, unusable output buffer.
 */
bool file_hex_decode(const char *encoded, char *text, size_t capacity);
/* Validate the bounded basename rules in docs/PROTOCOL.md, without encoding. */
bool file_name_valid(const char *name);
/* Decode and validate a basename into FILE_NAME_CAP caller-owned bytes. */
bool file_name_decode(const char *encoded, char name[FILE_NAME_CAP]);
/* line has length bytes plus trailing NUL; tokens has expected pointer slots.
 * Validate every raw byte/count, then replace single-space separators with NUL.
 * Returned pointers borrow line. Failure does not tokenize the line.
 */
bool file_tokens(char *line, size_t length, char **tokens, size_t expected);
/* Recognize a whole NUL-terminated ERR line, using length to reject hidden bytes. */
bool file_error(const char *line, size_t length);
/* Returned socket is caller-owned and watched when signals is non-NULL. Close
 * it with client_signals_close_socket. Failure returns -1/errno, including
 * ECANCELED when interrupted. With signals, call from the main thread and keep
 * the controller alive until closing the socket. NULL disables cancellation.
 */
int file_connect(const char *ip, uint16_t port, ClientSignals *signals);
/* Set receive/send inactivity timeouts on borrowed fd; 0 or -1/errno. Does not
 * close it, including after a partially applied timeout configuration.
 */
int file_socket_timeout(int fd);

#endif
