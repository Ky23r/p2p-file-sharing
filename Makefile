CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS = -pthread
LDLIBS = -lcrypto

SERVER_SOURCES = src/server/server.c src/server/auth.c src/server/session.c src/common/net_io.c
SERVER_OBJECTS = $(SERVER_SOURCES:src/%.c=build/%.o)
NET_TEST = build/tests/test_net_io

.PHONY: all clean test
all: server

server: $(SERVER_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(NET_TEST): tests/server/test_net_io.c build/common/net_io.o include/net_io.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/common/net_io.o $(LDFLAGS) -o $@

test: server $(NET_TEST)
	./$(NET_TEST)
	./server --help
	python3 -m unittest discover -s tests/server -p 'test_server.py' -v

clean:
	rm -rf build server

-include $(SERVER_OBJECTS:.o=.d)
