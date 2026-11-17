#include "client_signals.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define OUTBOUND_SOCKET_LIMIT 2U

static const int handled_signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE};
#define HANDLED_SIGNAL_COUNT (sizeof(handled_signals) / sizeof(handled_signals[0]))

struct ClientSignals {
    int notification[2];
    struct sigaction previous[HANDLED_SIGNAL_COUNT];
    size_t installed;
};

/* Workers inherit blocked signals. Only the main thread and its handler access
 * descriptor slots; the heartbeat also reads the lock-free signal number.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal notification needs lock-free atomic int");
static atomic_int signal_number;
static volatile sig_atomic_t notification_fd = -1;
static volatile sig_atomic_t outbound_sockets[OUTBOUND_SOCKET_LIMIT] = {-1, -1};

static void handle_signal(int number)
{
    int saved_errno = errno;
    if (atomic_load_explicit(&signal_number, memory_order_relaxed) == 0) {
        atomic_store_explicit(&signal_number, number, memory_order_relaxed);
        for (size_t i = 0; i < OUTBOUND_SOCKET_LIMIT; ++i) {
            int fd = outbound_sockets[i];
            if (fd >= 0)
                (void)shutdown(fd, SHUT_RDWR);
        }
        if (notification_fd >= 0) {
            unsigned char wake = 1;
            ssize_t written;
            do {
                written = write(notification_fd, &wake, sizeof(wake));
            } while (written < 0 && errno == EINTR);
        }
    }
    errno = saved_errno;
}

int client_signals_block_workers(sigset_t *previous_mask)
{
    sigset_t mask;
    sigemptyset(&mask);
    for (size_t i = 0; i < HANDLED_SIGNAL_COUNT; ++i)
        sigaddset(&mask, handled_signals[i]);
    int error = pthread_sigmask(SIG_BLOCK, &mask, previous_mask);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

ClientSignals *client_signals_start(void)
{
    ClientSignals *signals = calloc(1, sizeof(*signals));
    if (signals == NULL)
        return NULL;
    signals->notification[0] = signals->notification[1] = -1;
    sigset_t previous_mask;
    if (client_signals_block_workers(&previous_mask) != 0) {
        free(signals);
        return NULL;
    }
    if (notification_fd >= 0) {
        free(signals);
        pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
        errno = EBUSY;
        return NULL;
    }
    if (pipe(signals->notification) != 0 ||
        fcntl(signals->notification[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(signals->notification[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(signals->notification[1], F_SETFL, O_NONBLOCK) < 0)
        goto fail;
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigfillset(&action.sa_mask);
    for (size_t i = 0; i < HANDLED_SIGNAL_COUNT; ++i) {
        if (sigaction(handled_signals[i], &action, &signals->previous[i]) != 0)
            goto fail;
        signals->installed++;
    }
    atomic_store_explicit(&signal_number, 0, memory_order_relaxed);
    notification_fd = signals->notification[1];
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    return signals;
fail:;
    int error = errno;
    while (signals->installed != 0) {
        size_t i = --signals->installed;
        sigaction(handled_signals[i], &signals->previous[i], NULL);
    }
    if (signals->notification[0] >= 0)
        close(signals->notification[0]);
    if (signals->notification[1] >= 0)
        close(signals->notification[1]);
    free(signals);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    errno = error;
    return NULL;
}

int client_signals_stop(ClientSignals *signals)
{
    if (signals == NULL)
        return 0;
    sigset_t previous_mask;
    client_signals_block_workers(&previous_mask);
    int number = atomic_load_explicit(&signal_number, memory_order_relaxed);
    notification_fd = -1;
    for (size_t i = 0; i < OUTBOUND_SOCKET_LIMIT; ++i)
        outbound_sockets[i] = -1;
    for (size_t i = 0; i < signals->installed; ++i)
        sigaction(handled_signals[i], &signals->previous[i], NULL);
    close(signals->notification[0]);
    close(signals->notification[1]);
    free(signals);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    return number;
}

int client_signals_fd(const ClientSignals *signals)
{
    return signals == NULL ? -1 : signals->notification[0];
}

int client_signals_number(const ClientSignals *signals)
{
    return signals == NULL ? 0 : atomic_load_explicit(&signal_number, memory_order_relaxed);
}

int client_signals_watch_socket(ClientSignals *signals, int fd)
{
    if (signals == NULL)
        return 0;
    if (client_signals_number(signals) != 0) {
        errno = ECANCELED;
        return -1;
    }
    for (size_t i = 0; i < OUTBOUND_SOCKET_LIMIT; ++i)
        if (outbound_sockets[i] < 0) {
            outbound_sockets[i] = fd;
            return 0;
        }
    errno = ENOSPC;
    return -1;
}

void client_signals_close_socket(ClientSignals *signals, int fd)
{
    if (fd < 0)
        return;
    if (signals != NULL)
        for (size_t i = 0; i < OUTBOUND_SOCKET_LIMIT; ++i)
            if (outbound_sockets[i] == fd)
                outbound_sockets[i] = -1;
    close(fd);
}
