CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS = -pthread
LDLIBS = -lcrypto

SERVER_SOURCES = src/server/server.c src/server/auth.c src/server/session.c src/common/net_io.c
SERVER_OBJECTS = $(SERVER_SOURCES:src/%.c=build/%.o)
CLIENT_SOURCES = src/client/client.c src/client/config.c src/client/peer_server.c \
                 src/client/file_protocol.c src/client/shared_index.c \
                 src/client/transfer.c src/client/client_session.c src/common/net_io.c
CLIENT_OBJECTS = $(CLIENT_SOURCES:src/%.c=build/%.o)
NET_TEST = build/tests/test_net_io
FILES_TEST = build/tests/test_files

.PHONY: all clean test
all: server client

server: $(SERVER_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

client: $(CLIENT_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(NET_TEST): tests/server/test_net_io.c build/common/net_io.o include/net_io.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/common/net_io.o $(LDFLAGS) -o $@

$(FILES_TEST): tests/client/test_files.c build/client/file_protocol.o build/client/shared_index.o build/client/peer_server.o build/client/transfer.o build/common/net_io.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/client $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

test: server client $(NET_TEST) $(FILES_TEST)
	./$(NET_TEST)
	./$(FILES_TEST)
	./server --help
	python3 -m unittest discover -s tests/server -p 'test_server.py' -v
	python3 -m unittest discover -s tests/client -p 'test_client.py' -v

clean:
	rm -rf build server client

-include $(sort $(SERVER_OBJECTS:.o=.d) $(CLIENT_OBJECTS:.o=.d))
