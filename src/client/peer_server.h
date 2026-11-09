#ifndef P2P_PEER_SERVER_H
#define P2P_PEER_SERVER_H

#include <stdint.h>

/* Open an IPv4 TCP listener. The caller owns the returned descriptor.
 * Returns -1 with errno on failure.
 */
int open_peer_listener(uint16_t port);

#endif
