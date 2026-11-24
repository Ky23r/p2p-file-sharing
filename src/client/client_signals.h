#ifndef P2P_CLIENT_SIGNALS_H
#define P2P_CLIENT_SIGNALS_H

#include <signal.h>

typedef struct ClientSignals ClientSignals;

/* One controller per process, created/destroyed on the main thread. Owns its
 * notification pipe and handles SIGHUP, SIGINT, SIGQUIT, SIGTERM, and SIGPIPE
 * until stop restores the previous dispositions. Returns NULL with errno on
 * initialization failure.
 * Workers must inherit blocked handled signals; see block_workers below.
 */
ClientSignals *client_signals_start(void);
/* Call after joining workers and closing watched sockets. Blocks handled signals
 * before taking the final signal number, closes the pipe, and restores handlers
 * and the caller's mask. Returns that number (zero for no signal/NULL).
 * Signals pending/generated after this final block use the restored dispositions.
 */
int client_signals_stop(ClientSignals *signals);

/* Borrowed readable descriptor: never close or drain it. Remains readable after
 * cancellation until stop. number returns the first received signal or zero.
 * NULL disables cancellation: fd returns -1 and number returns zero.
 */
int client_signals_fd(const ClientSignals *signals);
int client_signals_number(const ClientSignals *signals);

/* Main-thread-only tracking of two borrowed outbound sockets (control/download).
 * watch returns 0 or -1/errno. Cancellation shuts them down without closing.
 * close_socket unregisters before closing, preventing descriptor-reuse races.
 * NULL gives ordinary ownership without cancellation; negative fd is safe.
 */
int client_signals_watch_socket(ClientSignals *signals, int fd);
void client_signals_close_socket(ClientSignals *signals, int fd);

/* Block handled signals before pthread_create so workers inherit that mask.
 * Returns 0 or -1/errno. The creator must restore previous_mask afterward.
 */
int client_signals_block_workers(sigset_t *previous_mask);

#endif
