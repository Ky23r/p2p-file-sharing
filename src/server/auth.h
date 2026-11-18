#ifndef P2P_AUTH_H
#define P2P_AUTH_H

#include <pthread.h>
#include <stdbool.h>

typedef struct account account;
typedef struct {
    pthread_mutex_t mutex;
    account *accounts;
    int fd;
    bool healthy;
} auth_store;

enum auth_result { AUTH_OK, AUTH_INVALID, AUTH_USER_EXISTS, AUTH_BAD_CREDENTIALS, AUTH_ERROR };
/* Exclusive lifetime lock; 0600 regular file owned by this effective UID.
 * Missing file is created; missing parent or invalid records fail closed.
 * init/destroy run outside worker lifetime. Failure sets errno.
 */
int auth_init(auth_store *store, const char *path);
void auth_destroy(auth_store *store);
enum auth_result auth_register(auth_store *store, const char *username, const char *password);
enum auth_result auth_login(auth_store *store, const char *username, const char *password);

#endif
