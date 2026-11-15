#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ID_DIGITS 10

static int read_id(const char *path, uint32_t *id)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct stat info;
    FILE *file = NULL;
    if (fstat(fd, &info) == 0) {
        if (S_ISREG(info.st_mode))
            file = fdopen(fd, "r");
        else
            errno = EINVAL;
    }
    if (file == NULL) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    char text[ID_DIGITS + sizeof("\r\n")];
    size_t length = fread(text, 1, sizeof(text), file);
    int error = ferror(file) ? errno : 0;
    if (fclose(file) != 0 && error == 0)
        error = errno;
    if (error != 0) {
        errno = error;
        return -1;
    }
    if (length == sizeof(text)) {
        errno = EINVAL;
        return -1;
    }
    if (length > 0 && text[length - 1] == '\n') {
        --length;
        if (length > 0 && text[length - 1] == '\r')
            --length;
    }
    text[length] = '\0';
    if (length == 0 || length > ID_DIGITS || strspn(text, "0123456789") != length) {
        errno = EINVAL;
        return -1;
    }

    uintmax_t number = strtoumax(text, NULL, 10);
    if (number > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    *id = (uint32_t)number;
    return 0;
}

static int create_id(const char *path, uint32_t *id)
{
    const char suffix[] = ".tmp.XXXXXX";
    size_t size = strlen(path) + sizeof(suffix);
    char *temporary = malloc(size);
    if (temporary == NULL)
        return -1;
    snprintf(temporary, size, "%s%s", path, suffix);

    int fd = mkstemp(temporary);
    int error = 0;
    if (fd < 0) {
        error = errno;
    } else {
        uint32_t candidate;
        if (RAND_bytes((unsigned char *)&candidate, sizeof(candidate)) != 1)
            error = EIO;
        else if (dprintf(fd, "%" PRIu32 "\n", candidate) < 0 || fsync(fd) < 0)
            error = errno;
        if (close(fd) < 0 && error == 0)
            error = errno;

        /* Publish the complete file without replacing another client's ID. */
        if (error == 0) {
            if (link(temporary, path) == 0)
                *id = candidate;
            else
                error = errno;
        }
        unlink(temporary);
    }
    free(temporary);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int load_client_id(const char *path, uint32_t *id)
{
    if (read_id(path, id) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;
    if (create_id(path, id) == 0)
        return 0;
    if (errno == EEXIST)
        return read_id(path, id);
    return -1;
}
