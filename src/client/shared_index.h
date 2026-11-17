#ifndef P2P_SHARED_INDEX_H
#define P2P_SHARED_INDEX_H

#include "file_protocol.h"

typedef struct {
    char name[FILE_NAME_CAP];
    char path[FILE_PATH_CAP];
    uint64_t size;
    bool announced;
} shared_file;

typedef struct shared_index shared_index;

shared_index *shared_index_load(const char *path);
void shared_index_destroy(shared_index *index);
int shared_index_add(shared_index *index, const char *path, char name[FILE_NAME_CAP]);
int shared_index_remove(shared_index *index, const char *name);
bool shared_index_get(shared_index *index, size_t position, shared_file *entry);
void shared_index_set_online(shared_index *index, bool online);
void shared_index_mark(shared_index *index, const char *name, bool announced, uint64_t size);
/* Open regular source, bound size, return caller-owned fd or -1/errno. */
int shared_file_open(const char *path, uint64_t *size);
/* Authorization and open are atomic with respect to logout/unshare. */
int shared_index_open(shared_index *index, const char *name, uint64_t *size);

#endif
