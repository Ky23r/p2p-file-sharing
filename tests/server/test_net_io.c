#include "net_io.h"

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
    char *line = NULL;
    size_t capacity = 0;
    size_t size;
    pair(fds);
    net_reader_init(&reader, fds[1]);
    const char input[] = "one\r\ntwo\r\n\r\nx\0y\r\nbare\nlast\r\n";
    assert(send_all(fds[0], input, sizeof(input) - 1) == (ssize_t)sizeof(input) - 1);
    assert(shutdown(fds[0], SHUT_WR) == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == 3 && strcmp(line, "one") == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == 3 && strcmp(line, "two") == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK && size == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == 3 && memcmp(line, "x\0y", 3) == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_INVALID);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == 4 && strcmp(line, "last") == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_EOF);
    free(line);
    close(fds[0]);
    close(fds[1]);
}

static void test_long_line(void)
{
    int fds[2];
    char input[16386];
    char *line = NULL;
    size_t capacity = 0, size;
    net_reader reader;
    pair(fds);
    memset(input, 'a', sizeof(input) - 2);
    memcpy(input + sizeof(input) - 2, "\r\n", 2);
    assert(send_all(fds[0], input, sizeof(input)) == sizeof(input));
    net_reader_init(&reader, fds[1]);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == sizeof(input) - 2 && line[size] == '\0');
    assert(memcmp(line, input, size) == 0);
    free(line);
    close(fds[0]);
    close(fds[1]);
}

static void test_errors(void)
{
    int fds[2];
    net_reader reader;
    char *line = NULL;
    size_t capacity = 0;
    size_t size;
    pair(fds);
    assert(send_all(fds[0], "partial", 7) == 7);
    close(fds[0]);
    net_reader_init(&reader, fds[1]);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_TRUNCATED);
    assert(send_all(fds[1], "x", 1) == -1); /* must not raise SIGPIPE */
    close(fds[1]);
    net_reader_init(&reader, -1);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_ERROR && errno == EBADF);
    free(line);
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
    assert(send_all(args->fd, "mented\r", 7) == 7);
    pause_briefly();
    assert(send_all(args->fd, "\nnext\r\n", 7) == 7);
    return NULL;
}

static void test_fragmentation_and_eintr(void)
{
    int fds[2];
    pthread_t writer;
    net_reader reader;
    char *line = NULL;
    size_t capacity = 0;
    size_t size;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    pair(fds);
    writer_args args = {fds[0], pthread_self()};
    net_reader_init(&reader, fds[1]);
    assert(pthread_create(&writer, NULL, fragment_writer, &args) == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(size == 10 && strcmp(line, "fragmented") == 0);
    assert(recv_line(&reader, &line, &capacity, &size) == NET_LINE_OK);
    assert(strcmp(line, "next") == 0 && signals_seen > 0);
    free(line);
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
    char *header = NULL;
    size_t capacity = 0;
    size_t length;
    unsigned char body[8];
    const unsigned char wire[] = "200 5\r\n\0\nabc";
    pair(fds);
    net_reader_init(&reader, fds[1]);
    assert(send_all(fds[0], wire, sizeof(wire) - 1) == (ssize_t)sizeof(wire) - 1);
    assert(shutdown(fds[0], SHUT_WR) == 0);
    assert(recv_line(&reader, &header, &capacity, &length) == NET_LINE_OK);
    assert(strcmp(header, "200 5") == 0);
    free(header);
    assert(net_read(&reader, body, 2) == 2);
    assert(net_read(&reader, body + 2, sizeof(body) - 2) == 3);
    assert(memcmp(body, "\0\nabc", 5) == 0);
    assert(net_read(&reader, body, sizeof(body)) == 0);
    assert(net_read(&reader, NULL, 0) == 0);
    assert(net_read(&reader, NULL, 1) == -1 && errno == EINVAL);
    close(fds[0]);
    close(fds[1]);
}

static void test_bounded_line(void)
{
    int fds[2];
    pair(fds);
    net_reader reader;
    net_reader_init(&reader, fds[1]);
    const char wire[] = "abc\r\nabcd";
    assert(send_all(fds[0], wire, sizeof(wire) - 1) == (ssize_t)sizeof(wire) - 1);
    char *line = NULL;
    size_t capacity = 0, length;
    assert(recv_line_bounded(&reader, &line, &capacity, &length, 4) == NET_LINE_OK);
    assert(length == 3 && strcmp(line, "abc") == 0);
    assert(recv_line_bounded(&reader, &line, &capacity, &length, 3) == NET_LINE_TOO_LONG);
    free(line);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_lines();
    test_long_line();
    test_errors();
    test_fragmentation_and_eintr();
    test_partial_send();
    test_header_followed_by_binary_body();
    test_bounded_line();
    puts("net_io: 7 test groups passed (buffering, long lines, errors, EINTR, partial sends, binary body, line limit)");
    return 0;
}
