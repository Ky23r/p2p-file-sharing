#define _XOPEN_SOURCE 700
#include "shared_index.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct shared_index {
    pthread_mutex_t mutex;
    char *path;
    int lock_fd;
    size_t count;
    bool online;
    shared_file entries[FILE_ENTRY_LIMIT];
};

static size_t find_entry(shared_index *index, const char *name)
{
    size_t i = 0;
    while (i < index->count && strcmp(index->entries[i].name, name) != 0)
        ++i;
    return i;
}

int shared_file_open(const char *path, uint64_t *size)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat info;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        (uint64_t)info.st_size <= FILE_SIZE_MAX) {
        *size = (uint64_t)info.st_size;
        return fd;
    }
    close(fd);
    errno = EINVAL;
    return -1;
}

/* index mutex held; skip selects the entry removed by this transaction. */
static int save_index(shared_index *index, size_t skip)
{
    size_t capacity = strlen(index->path) + sizeof(".tmp.XXXXXX");
    char *temporary = malloc(capacity);
    if (temporary == NULL)
        return -1;
    snprintf(temporary, capacity, "%s.tmp.XXXXXX", index->path);
    int fd = mkstemp(temporary);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
    int error = 0;
    if (file == NULL) {
        error = errno;
        if (fd >= 0)
            close(fd);
    } else {
        for (size_t i = 0; i < index->count && error == 0; ++i) {
            if (i == skip)
                continue;
            char name[FILE_NAME_HEX_CAP], path[FILE_PATH_CAP * 2];
            file_hex_encode(index->entries[i].name, name, sizeof(name));
            file_hex_encode(index->entries[i].path, path, sizeof(path));
            if (fprintf(file, "%s\t%s\n", name, path) < 0)
                error = errno;
        }
        if (error == 0 && (fflush(file) != 0 || fsync(fd) != 0))
            error = errno;
        if (fclose(file) != 0 && error == 0)
            error = errno;
        if (error == 0 && rename(temporary, index->path) != 0)
            error = errno;
    }
    if (fd >= 0)
        unlink(temporary);
    free(temporary);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static bool valid_path(const char *path)
{
    if (*path != '/')
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if (*p < 0x20 || *p == 0x7f)
            return false;
    return true;
}

static int read_index(shared_index *index)
{
    int fd = open(index->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (file == NULL) {
        close(fd);
        return -1;
    }
    char line[2 * (FILE_NAME_CAP + FILE_PATH_CAP) + 2];
    int result = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        char *separator = strchr(line, '\t');
        if (index->count == FILE_ENTRY_LIMIT || length == 0 || line[length - 1] != '\n' || separator == NULL) {
            result = -1;
            break;
        }
        line[length - 1] = '\0';
        *separator++ = '\0';
        shared_file *entry = &index->entries[index->count];
        if (!file_name_decode(line, entry->name) ||
            !file_hex_decode(separator, entry->path, sizeof(entry->path)) || !valid_path(entry->path) ||
            strcmp(strrchr(entry->path, '/') + 1, entry->name) != 0 || find_entry(index, entry->name) != index->count) {
            result = -1;
            break;
        }
        int source = shared_file_open(entry->path, &entry->size);
        if (source >= 0)
            close(source);
        index->count++;
    }
    if (ferror(file))
        result = -1;
    if (fclose(file) != 0)
        result = -1;
    if (result != 0)
        errno = EINVAL;
    return result;
}

shared_index *shared_index_load(const char *path)
{
    shared_index *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return NULL;
    int error = pthread_mutex_init(&index->mutex, NULL);
    if (error != 0) {
        free(index);
        errno = error;
        return NULL;
    }
    index->lock_fd = -1;
    index->path = strdup(path);
    char *lock_path = malloc(strlen(path) + sizeof(".lock"));
    if (index->path == NULL || lock_path == NULL) {
        free(lock_path);
        goto fail;
    }
    sprintf(lock_path, "%s.lock", path);
    index->lock_fd = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    free(lock_path);
    struct stat info;
    if (index->lock_fd < 0)
        goto fail;
    if (fstat(index->lock_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) {
        errno = EINVAL;
        goto fail;
    }
    if (flock(index->lock_fd, LOCK_EX | LOCK_NB) != 0 || read_index(index) != 0)
        goto fail;
    return index;
fail:
    error = errno;
    shared_index_destroy(index);
    errno = error;
    return NULL;
}

void shared_index_destroy(shared_index *index)
{
    if (index == NULL)
        return;
    if (index->lock_fd >= 0)
        close(index->lock_fd);
    pthread_mutex_destroy(&index->mutex);
    free(index->path);
    free(index);
}

int shared_index_add(shared_index *index, const char *path, char name[FILE_NAME_CAP])
{
    char canonical[FILE_PATH_CAP];
    if (realpath(path, canonical) == NULL)
        return -1;
    const char *basename = strrchr(canonical, '/') + 1;
    if (!valid_path(canonical) || !file_name_valid(basename)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t size;
    int fd = shared_file_open(canonical, &size);
    if (fd < 0)
        return -1;
    close(fd);
    strcpy(name, basename);
    pthread_mutex_lock(&index->mutex);
    size_t position = find_entry(index, name);
    int result = 0;
    if (position != index->count) {
        if (strcmp(index->entries[position].path, canonical) != 0) {
            errno = EEXIST;
            result = -1;
        }
    } else if (index->count == FILE_ENTRY_LIMIT) {
        errno = ENOSPC;
        result = -1;
    } else {
        shared_file *entry = &index->entries[index->count++];
        strcpy(entry->name, name);
        strcpy(entry->path, canonical);
        entry->size = size;
        entry->announced = false;
        if (save_index(index, FILE_ENTRY_LIMIT) != 0) {
            index->count--;
            result = -1;
        }
    }
    pthread_mutex_unlock(&index->mutex);
    return result;
}

int shared_index_remove(shared_index *index, const char *name)
{
    pthread_mutex_lock(&index->mutex);
    size_t position = find_entry(index, name);
    int result = 0;
    if (position < index->count) {
        index->entries[position].announced = false;
        result = save_index(index, position);
        if (result == 0) {
            memmove(index->entries + position, index->entries + position + 1,
                    (index->count - position - 1) * sizeof(shared_file));
            index->count--;
        }
    }
    pthread_mutex_unlock(&index->mutex);
    return result;
}

bool shared_index_get(shared_index *index, size_t position, shared_file *entry)
{
    pthread_mutex_lock(&index->mutex);
    bool found = position < index->count;
    if (found)
        *entry = index->entries[position];
    pthread_mutex_unlock(&index->mutex);
    return found;
}

void shared_index_set_online(shared_index *index, bool online)
{
    pthread_mutex_lock(&index->mutex);
    index->online = online;
    if (!online)
        for (size_t i = 0; i < index->count; ++i)
            index->entries[i].announced = false;
    pthread_mutex_unlock(&index->mutex);
}

void shared_index_mark(shared_index *index, const char *name, bool announced, uint64_t size)
{
    pthread_mutex_lock(&index->mutex);
    size_t position = find_entry(index, name);
    if (position < index->count) {
        index->entries[position].announced = announced;
        index->entries[position].size = size;
    }
    pthread_mutex_unlock(&index->mutex);
}

int shared_index_open(shared_index *index, const char *name, uint64_t *size)
{
    pthread_mutex_lock(&index->mutex);
    size_t position = find_entry(index, name);
    int fd = -1;
    errno = EACCES;
    if (index->online && position < index->count && index->entries[position].announced) {
        fd = shared_file_open(index->entries[position].path, size);
        if (fd >= 0 && *size != index->entries[position].size) {
            close(fd);
            fd = -1;
            errno = ESTALE;
        }
    }
    pthread_mutex_unlock(&index->mutex);
    return fd;
}
