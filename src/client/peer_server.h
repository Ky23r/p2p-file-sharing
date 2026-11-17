#ifndef P2P_PEER_SERVER_H
#define P2P_PEER_SERVER_H

#include <stdint.h>
#include "shared_index.h"

/* Open an IPv4 TCP listener. The caller owns the returned descriptor.
 * Returns -1 with errno on failure.
 */
int open_peer_listener(uint16_t port);

typedef struct peer_server peer_server;
/* Listener remains caller-owned and must outlive the workers. */
peer_server *peer_server_start(int listener, shared_index *index);
void peer_server_stop(peer_server *server);

#endif
