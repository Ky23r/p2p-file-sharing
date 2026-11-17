#ifndef P2P_PEER_SERVER_H
#define P2P_PEER_SERVER_H

#include <stdint.h>
#include "shared_index.h"

/* Open an IPv4 TCP listener. The caller owns the returned descriptor.
 * Returns -1 with errno on failure.
 */
int open_peer_listener(uint16_t port);

typedef struct PeerServer PeerServer;
/* Start bounded upload workers, borrowing listener/index until stop. Returns
 * owned worker state or NULL/errno. Sets listener nonblocking, including possibly
 * on failure; never closes that caller-owned listener or destroys the index.
 */
PeerServer *peer_server_start(int listener, SharedIndex *index);
/* Interrupt active sockets, join every worker, and free state. NULL is safe.
 * Owner-thread operation: do not call concurrently with another stop.
 */
void peer_server_stop(PeerServer *server);

#endif
