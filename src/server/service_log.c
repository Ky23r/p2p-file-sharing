#include "service_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int service_log_init(service_log *log, const char *path)
{
    int error = pthread_mutex_init(&log->mutex, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    log->fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (log->fd < 0)
        goto fail;
    struct stat info;
    if (fstat(log->fd, &info) != 0)
        goto close_file;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) {
        errno = EINVAL;
        goto close_file;
    }
    if (fchmod(log->fd, 0600) != 0)
        goto close_file;
    return 0;

close_file:
    error = errno;
    close(log->fd);
    errno = error;
fail:
    error = errno;
    pthread_mutex_destroy(&log->mutex);
    errno = error;
    return -1;
}

void service_log_write(service_log *log, const char *format, ...)
{
    int saved = errno;
    char line[2048];
    time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL) {
        errno = saved;
        return;
    }
    size_t used = strftime(line, sizeof(line), "%Y-%m-%dT%H:%M:%SZ ", &utc);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line + used, sizeof(line) - used, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(line) - used - 1) {
        errno = saved;
        return;
    }
    used += (size_t)length;
    line[used++] = '\n';
    pthread_mutex_lock(&log->mutex);
    size_t sent = 0;
    while (sent < used) {
        ssize_t n = write(log->fd, line + sent, used - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    pthread_mutex_unlock(&log->mutex);
    errno = saved;
}

void service_log_destroy(service_log *log)
{
    close(log->fd);
    pthread_mutex_destroy(&log->mutex);
}
