#ifndef P2P_SHARED_INDEX_H
#define P2P_SHARED_INDEX_H

#include "file_protocol.h"

typedef struct {
    char name[FILE_NAME_CAP];
    char path[FILE_PATH_CAP];
    uint64_t size;
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
 * Returns 0 or -1/errno; name needs FILE_NAME_CAP bytes and is valid on success.
 * Same name/path is idempotent; a different path with that name gives EEXIST.
 */
int shared_index_add(SharedIndex *index, const char *path, char name[FILE_NAME_CAP]);
/* Idempotent removal, 0 or -1/errno. On save failure retain the entry inactive;
 * its old on-disk intent may be republished after a later login/restart.
 */
int shared_index_remove(SharedIndex *index, const char *name);
/* Copy entry at zero-based position; false means it is outside the catalog. */
bool shared_index_get(SharedIndex *index, size_t position, SharedFile *entry);
/* Going offline revokes all announcements. Going online alone activates none. */
void shared_index_set_online(SharedIndex *index, bool online);
/* Update a known entry's announcement/advertised size; unknown name is ignored.
 * Only set announced=true after the corresponding server acknowledgement.
 */
void shared_index_mark(SharedIndex *index, const char *name, bool announced, uint64_t size);
/* Open a regular source without following its final symlink. Return caller-owned
 * fd or -1/errno. size is valid only on success.
 */
int shared_file_open(const char *path, uint64_t *size);
/* Require online/announced state and unchanged advertised size. Authorization
 * and opening are atomic with logout/unshare; fd remains usable afterward.
 * Returns caller-owned fd or -1/errno (ESTALE for changed size).
 */
int shared_index_open(SharedIndex *index, const char *name, uint64_t *size);

#endif
