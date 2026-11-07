#include "auth.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define SALT_SIZE 16
#define HASH_SIZE 32
#define HASH_ITERATIONS 600000
#define RECORD_SIZE 160

struct account {
    char username[USERNAME_MAX + 1];
    unsigned char salt[SALT_SIZE];
    unsigned char hash[HASH_SIZE];
    struct account *next;
};

bool auth_valid_username(const char *username)
{
    size_t length = strlen(username);
    if (length < USERNAME_MIN || length > USERNAME_MAX)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

bool auth_valid_password(const char *password)
{
    size_t length = strlen(password);
    if (length < PASSWORD_MIN || length > PASSWORD_MAX)
        return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)password[i] < 0x21 || (unsigned char)password[i] > 0x7e)
            return false;
    return true;
}

static account *find_account(auth_store *store, const char *username)
{
    for (account *entry = store->accounts; entry != NULL; entry = entry->next)
        if (strcmp(entry->username, username) == 0)
            return entry;
    return NULL;
}

static int hash_password(const char *password, const unsigned char salt[SALT_SIZE],
                         unsigned char hash[HASH_SIZE])
{
    return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, SALT_SIZE,
                             HASH_ITERATIONS, EVP_sha256(), HASH_SIZE, hash) == 1 ? 0 : -1;
}

static int unhex(const char *text, unsigned char *output, size_t size)
{
    if (strlen(text) != size * 2)
        return -1;
    for (size_t i = 0; i < size; ++i) {
        unsigned int byte = 0;
        for (size_t j = 0; j < 2; ++j) {
            char c = text[2 * i + j];
            unsigned int digit;
            if (c >= '0' && c <= '9')
                digit = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = (unsigned int)(c - 'a') + 10;
            else
                return -1;
            byte = byte * 16 + digit;
        }
        output[i] = (unsigned char)byte;
    }
    return 0;
}

static void hex(const unsigned char *bytes, size_t size, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    output[size * 2] = '\0';
}

static int load_record(auth_store *store, char *line)
{
    char *fields[4] = {line, NULL, NULL, NULL};
    for (size_t i = 1; i < 4; ++i) {
        char *separator = strchr(fields[i - 1], ':');
        if (separator == NULL)
            return -1;
        *separator = '\0';
        fields[i] = separator + 1;
    }
    if (!auth_valid_username(fields[0]) || strcmp(fields[1], "600000") != 0 ||
        find_account(store, fields[0]) != NULL)
        return -1;
    account *entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
        return -1;
    if (unhex(fields[2], entry->salt, SALT_SIZE) != 0 ||
        unhex(fields[3], entry->hash, HASH_SIZE) != 0) {
        OPENSSL_cleanse(entry, sizeof(*entry));
        free(entry);
        return -1;
    }
    strcpy(entry->username, fields[0]);
    entry->next = store->accounts;
    store->accounts = entry;
    return 0;
}

static int load_accounts(auth_store *store)
{
    int copy = dup(store->fd);
    if (copy < 0)
        return -1;
    FILE *file = fdopen(copy, "r");
    if (file == NULL) {
        close(copy);
        return -1;
    }
    char line[RECORD_SIZE];
    size_t length = 0;
    int result = 0;
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (c == '\n') {
            line[length] = '\0';
            if (load_record(store, line) != 0) {
                result = -1;
                break;
            }
            length = 0;
        } else if (c < 0x21 || c > 0x7e || length == sizeof(line) - 1) {
            result = -1;
            break;
        } else {
            line[length++] = (char)c;
        }
    }
    if (length != 0 || ferror(file))
        result = -1;
    if (fclose(file) != 0)
        result = -1;
    if (result != 0)
        errno = EINVAL;
    return result;
}

int auth_init(auth_store *store, const char *path)
{
    memset(store, 0, sizeof(*store));
    store->fd = -1;
    int error = pthread_mutex_init(&store->mutex, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }
    store->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (store->fd < 0)
        goto fail;
    struct stat info;
    if (fstat(store->fd, &info) != 0)
        goto fail;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) {
        errno = EACCES;
        goto fail;
    }
    if (flock(store->fd, LOCK_EX | LOCK_NB) != 0 || fchmod(store->fd, 0600) != 0)
        goto fail;
    if (load_accounts(store) != 0)
        goto fail;
    store->healthy = true;
    return 0;

fail: {
        int saved = errno;
        auth_destroy(store);
        errno = saved;
        return -1;
    }
}

void auth_destroy(auth_store *store)
{
    account *entry = store->accounts;
    while (entry != NULL) {
        account *next = entry->next;
        OPENSSL_cleanse(entry, sizeof(*entry));
        free(entry);
        entry = next;
    }
    if (store->fd >= 0)
        close(store->fd);
    pthread_mutex_destroy(&store->mutex);
}

/* Under the mutex: commit memory only after a durable append. Roll back a
 * failed/partial append; if rollback fails, reject further store operations.
 */
static int persist_account(auth_store *store, const account *entry)
{
    char salt[SALT_SIZE * 2 + 1];
    char hash[HASH_SIZE * 2 + 1];
    char record[RECORD_SIZE];
    hex(entry->salt, SALT_SIZE, salt);
    hex(entry->hash, HASH_SIZE, hash);
    int length = snprintf(record, sizeof(record), "%s:%d:%s:%s\n",
                          entry->username, HASH_ITERATIONS, salt, hash);
    if (length < 0 || (size_t)length >= sizeof(record))
        return -1;
    off_t start = lseek(store->fd, 0, SEEK_END);
    if (start < 0)
        return -1;
    size_t written = 0;
    while (written < (size_t)length) {
        ssize_t n = write(store->fd, record + written, (size_t)length - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        written += (size_t)n;
    }
    if (written == (size_t)length && fsync(store->fd) == 0)
        return 0;
    if (ftruncate(store->fd, start) != 0 || fsync(store->fd) != 0)
        store->healthy = false;
    return -1;
}

enum auth_result auth_register(auth_store *store, const char *username, const char *password)
{
    if (!auth_valid_username(username) || !auth_valid_password(password))
        return AUTH_INVALID;
    pthread_mutex_lock(&store->mutex);
    bool exists = find_account(store, username) != NULL;
    bool healthy = store->healthy;
    pthread_mutex_unlock(&store->mutex);
    if (!healthy)
        return AUTH_ERROR;
    if (exists)
        return AUTH_USER_EXISTS;
    account *entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
        return AUTH_ERROR;
    strcpy(entry->username, username);
    if (RAND_bytes(entry->salt, SALT_SIZE) != 1 ||
        hash_password(password, entry->salt, entry->hash) != 0) {
        OPENSSL_cleanse(entry, sizeof(*entry));
        free(entry);
        return AUTH_ERROR;
    }
    enum auth_result result;
    pthread_mutex_lock(&store->mutex);
    if (!store->healthy)
        result = AUTH_ERROR;
    else if (find_account(store, username) != NULL)
        result = AUTH_USER_EXISTS;
    else if (persist_account(store, entry) != 0)
        result = AUTH_ERROR;
    else {
        entry->next = store->accounts;
        store->accounts = entry;
        result = AUTH_OK;
    }
    pthread_mutex_unlock(&store->mutex);
    if (result != AUTH_OK) {
        OPENSSL_cleanse(entry, sizeof(*entry));
        free(entry);
    }
    return result;
}

enum auth_result auth_login(auth_store *store, const char *username, const char *password)
{
    unsigned char salt[SALT_SIZE] = {0};
    unsigned char expected[HASH_SIZE] = {0};
    unsigned char actual[HASH_SIZE] = {0};
    if (!auth_valid_username(username) || !auth_valid_password(password))
        return AUTH_INVALID;
    pthread_mutex_lock(&store->mutex);
    account *entry = find_account(store, username);
    bool exists = entry != NULL;
    bool healthy = store->healthy;
    if (exists) {
        memcpy(salt, entry->salt, sizeof(salt));
        memcpy(expected, entry->hash, sizeof(expected));
    }
    pthread_mutex_unlock(&store->mutex);
    if (!healthy)
        return AUTH_ERROR;
    /* Do the same KDF even for unknown users; never compare plaintext. */
    int hashed = hash_password(password, salt, actual);
    int difference = CRYPTO_memcmp(actual, expected, sizeof(actual));
    OPENSSL_cleanse(actual, sizeof(actual));
    OPENSSL_cleanse(expected, sizeof(expected));
    OPENSSL_cleanse(salt, sizeof(salt));
    if (hashed != 0)
        return AUTH_ERROR;
    return exists && difference == 0 ? AUTH_OK : AUTH_BAD_CREDENTIALS;
}
