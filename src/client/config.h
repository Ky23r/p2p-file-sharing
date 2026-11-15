#ifndef P2P_CLIENT_CONFIG_H
#define P2P_CLIENT_CONFIG_H

#include <stdint.h>

/* Load the saved ID or create one if the file is absent. Returns -1 with errno
 * on failure. An existing file is never overwritten.
 */
int load_client_id(const char *path, uint32_t *id);

#endif
