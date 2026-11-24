#ifndef P2P_FILE_PROTOCOL_H
#define P2P_FILE_PROTOCOL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "client_signals.h"
#include "protocol.h"

#define FILE_NAME_CAP (PROTOCOL_FILENAME_MAX + 1U)
#define FILE_NAME_HEX_CAP (2U * FILE_NAME_CAP)
#define FILE_NAME_TOKEN_CAP (PROTOCOL_FILENAME_TOKEN_MAX + 1U)
#define FILE_PATH_CAP PATH_MAX
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

/* Local catalog encoding only; wire values use the shared protocol.h helpers.
 * Encode bytes as lowercase hex; capacity includes the trailing NUL.
 */
bool file_hex_encode(const char *text, char *encoded, size_t capacity);
/* Decode lowercase hex, rejecting malformed input and decoded NULs. capacity
 * includes NUL; failure may leave a partly written, unusable output buffer.
 */
bool file_hex_decode(const char *encoded, char *text, size_t capacity);
/* Decode a local catalog's hex basename into FILE_NAME_CAP caller-owned bytes. */
bool file_name_decode(const char *encoded, char name[FILE_NAME_CAP]);
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
