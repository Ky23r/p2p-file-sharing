#ifndef P2P_SERVICE_LOG_H
#define P2P_SERVICE_LOG_H

#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    int fd;
} service_log;

/* Append-only, mode 0600 regular file owned by this user. init/destroy run
 * outside worker lifetime. Writes are best effort after successful startup.
 * Callers supply trusted formats and validated fields, never raw requests.
 */
int service_log_init(service_log *log, const char *path);
void service_log_write(service_log *log, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void service_log_destroy(service_log *log);

#endif
