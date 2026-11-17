#include "net_io.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void pair(int fds[2])
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
}

static void test_lines(void)
{
    int fds[2];
    net_reader reader;
    char line[PROTOCOL_MAX_LINE + 1];
    size_t size;
    pair(fds);
    net_reader_init(&reader, fds[1]);
    const char input[] = "one\ntwo\n\nx\0y\n";
    assert(send_all(fds[0], input, sizeof(input) - 1) == (ssize_t)sizeof(input) - 1);
    assert(shutdown(fds[0], SHUT_WR) == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK);
    assert(size == 3 && strcmp(line, "one") == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK);
    assert(size == 3 && strcmp(line, "two") == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK && size == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK);
    assert(size == 3 && memcmp(line, "x\0y", 3) == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_EOF);
    close(fds[0]);
    close(fds[1]);
}

static void test_boundaries(void)
{
    for (size_t count = PROTOCOL_MAX_LINE; count <= PROTOCOL_MAX_LINE + 1; ++count) {
        int fds[2];
        char input[PROTOCOL_MAX_LINE + 2];
        char line[PROTOCOL_MAX_LINE + 1];
        size_t size;
        net_reader reader;
        pair(fds);
        memset(input, 'a', count);
        input[count] = '\n';
        assert(send_all(fds[0], input, count + 1) == (ssize_t)count + 1);
        net_reader_init(&reader, fds[1]);
        int result = recv_line(&reader, line, sizeof(line), &size);
        assert(result == (count == PROTOCOL_MAX_LINE ? NET_LINE_OK : NET_LINE_TOO_LONG));
        if (result == NET_LINE_OK)
            assert(size == count && line[count] == '\0');
        close(fds[0]);
        close(fds[1]);
    }
}

static void test_errors(void)
{
    int fds[2];
    net_reader reader;
    char line[16];
    size_t size;
    pair(fds);
    assert(send_all(fds[0], "partial", 7) == 7);
    close(fds[0]);
    net_reader_init(&reader, fds[1]);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_TRUNCATED);
    assert(send_all(fds[1], "x", 1) == -1); /* must not raise SIGPIPE */
    close(fds[1]);
    net_reader_init(&reader, -1);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_ERROR && errno == EBADF);
    assert(recv_line(&reader, line, 0, &size) == NET_LINE_ERROR && errno == EINVAL);
    assert(send_all(-1, "x", 1) == -1 && errno == EBADF);
    assert(send_all(-1, NULL, 0) == 0);
    assert(send_all(-1, NULL, 1) == -1 && errno == EINVAL);
}

typedef struct {
    int fd;
    pthread_t target;
} writer_args;

static volatile sig_atomic_t signals_seen;

static void on_signal(int signal_number)
{
    (void)signal_number;
    signals_seen++;
}

static void pause_briefly(void)
{
    struct timespec delay = {0, 20000000};
    while (nanosleep(&delay, &delay) != 0)
        assert(errno == EINTR);
}

static void *fragment_writer(void *arg)
{
    writer_args *args = arg;
    pause_briefly();
    assert(pthread_kill(args->target, SIGUSR1) == 0);
    pause_briefly();
    assert(send_all(args->fd, "frag", 4) == 4);
    pause_briefly();
    assert(send_all(args->fd, "mented\nnext\n", 12) == 12);
    return NULL;
}

static void test_fragmentation_and_eintr(void)
{
    int fds[2];
    pthread_t writer;
    net_reader reader;
    char line[64];
    size_t size;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    pair(fds);
    writer_args args = {fds[0], pthread_self()};
    net_reader_init(&reader, fds[1]);
    assert(pthread_create(&writer, NULL, fragment_writer, &args) == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK);
    assert(size == 10 && strcmp(line, "fragmented") == 0);
    assert(recv_line(&reader, line, sizeof(line), &size) == NET_LINE_OK);
    assert(strcmp(line, "next") == 0 && signals_seen > 0);
    assert(pthread_join(writer, NULL) == 0);
    close(fds[0]);
    close(fds[1]);
}

#define TRANSFER_SIZE (2U * 1024U * 1024U)

static void *bulk_reader(void *arg)
{
    writer_args *args = arg;
    char buffer[997];
    size_t total = 0;
    pause_briefly();
    assert(pthread_kill(args->target, SIGUSR1) == 0);
    pause_briefly();
    while (total < TRANSFER_SIZE) {
        ssize_t count = recv(args->fd, buffer, sizeof(buffer), 0);
        assert(count > 0);
        for (ssize_t i = 0; i < count; ++i)
            assert(buffer[i] == 'z');
        total += (size_t)count;
    }
    assert(total == TRANSFER_SIZE);
    return NULL;
}

static void test_partial_send(void)
{
    int fds[2];
    int small = 1024;
    pthread_t thread;
    char *data = malloc(TRANSFER_SIZE);
    assert(data != NULL);
    memset(data, 'z', TRANSFER_SIZE);
    pair(fds);
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    writer_args args = {fds[1], pthread_self()};
    assert(pthread_create(&thread, NULL, bulk_reader, &args) == 0);
    assert(send_all(fds[0], data, TRANSFER_SIZE) == TRANSFER_SIZE);
    assert(pthread_join(thread, NULL) == 0);
    free(data);
    close(fds[0]);
    close(fds[1]);
}

static void test_header_followed_by_binary_body(void)
{
    int fds[2];
    net_reader reader;
    char header[32];
    size_t length;
    unsigned char body[8];
    const unsigned char wire[] = "OK FILE 5\n\0\nabc";
    pair(fds);
    net_reader_init(&reader, fds[1]);
    assert(send_all(fds[0], wire, sizeof(wire) - 1) == (ssize_t)sizeof(wire) - 1);
    assert(shutdown(fds[0], SHUT_WR) == 0);
    assert(recv_line(&reader, header, sizeof(header), &length) == NET_LINE_OK);
    assert(strcmp(header, "OK FILE 5") == 0);
    assert(net_read(&reader, body, 2) == 2);
    assert(net_read(&reader, body + 2, sizeof(body) - 2) == 3);
    assert(memcmp(body, "\0\nabc", 5) == 0);
    assert(net_read(&reader, body, sizeof(body)) == 0);
    assert(net_read(&reader, NULL, 0) == 0);
    assert(net_read(&reader, NULL, 1) == -1 && errno == EINVAL);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_lines();
    test_boundaries();
    test_errors();
    test_fragmentation_and_eintr();
    test_partial_send();
    test_header_followed_by_binary_body();
    puts("net_io: 6 test groups passed (buffering, limits, errors, EINTR, partial sends, binary body)");
    return 0;
}
