#ifndef P2P_SHARED_INDEX_H
#define P2P_SHARED_INDEX_H

#include "file_protocol.h"

typedef struct {
    char name[FILE_NAME_CAP];
    char path[FILE_PATH_CAP];
    bool announced;
} SharedFile;

typedef struct SharedIndex SharedIndex;

/* Operations lock internally; enumeration returns individual snapshots rather
 * than an atomic view across multiple calls. Strings are NUL-terminated and
 * pointers are non-NULL unless stated. Stop all users before destroy.
 */

/* Load or create an empty in-memory catalog, copying path and holding an
 * exclusive .lock until destroy. Returns owned state or NULL/errno. Invalid
 * existing catalog contents are never rewritten during load.
 */
SharedIndex *shared_index_load(const char *path);
/* Release the lock and state; NULL is safe. Does not delete persisted files. */
void shared_index_destroy(SharedIndex *index);
/* Open a canonical regular file with a wire-valid basename and atomically save it.
 * Returns 0 and copies the entry on success, or -1/errno.
 * Same name/path is idempotent; a different path with that name gives EEXIST.
 */
int shared_index_add(SharedIndex *index, const char *path, SharedFile *entry);
/* Idempotent removal, 0 or -1/errno. On save failure keep the entry and its
 * announcement unchanged. A successful removal prevents new opens.
 */
int shared_index_remove(SharedIndex *index, const char *name);
/* Copy entry at zero-based position; false means it is outside the catalog. */
bool shared_index_get(SharedIndex *index, size_t position, SharedFile *entry);
/* Going offline revokes all announcements. Going online alone activates none. */
void shared_index_set_online(SharedIndex *index, bool online);
/* Update a known entry's announcement after the server acknowledges it. */
void shared_index_mark(SharedIndex *index, const char *name, bool announced);
/* Open a regular source without following its final symlink. Returns a
 * caller-owned fd or -1/errno.
 */
int shared_file_open(const char *path);
/* Require online/announced state. Authorization and opening are atomic with
 * logout/unshare; fd remains usable afterward. Returns caller-owned fd or
 * -1/errno (ENODATA for an inactive share, or the source open error).
 */
int shared_index_open(SharedIndex *index, const char *name);
/* Whether a source open error confirms an announced file is stale. */
bool shared_file_stale(int error);

#endif
