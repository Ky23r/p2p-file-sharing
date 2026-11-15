CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS = -pthread
LDLIBS = -lcrypto

SERVER_SOURCES = src/server/server.c src/server/auth.c src/server/session.c src/server/service_log.c src/common/net_io.c src/common/protocol.c
SERVER_OBJECTS = $(SERVER_SOURCES:src/%.c=build/%.o)
CLIENT_SOURCES = src/client/client.c src/client/config.c src/client/peer_server.c src/common/net_io.c src/common/protocol.c
CLIENT_OBJECTS = $(CLIENT_SOURCES:src/%.c=build/%.o)
NET_TEST = build/tests/test_net_io
PROTOCOL_TEST = build/tests/test_protocol

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

$(PROTOCOL_TEST): tests/server/test_protocol.c build/common/protocol.o include/protocol.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< build/common/protocol.o $(LDFLAGS) -o $@

test: server client $(NET_TEST) $(PROTOCOL_TEST)
	./$(NET_TEST)
	./$(PROTOCOL_TEST)
	./server --help
	python3 -m unittest discover -s tests/server -p 'test_*.py' -v

clean:
	rm -rf build server client

-include $(sort $(SERVER_OBJECTS:.o=.d) $(CLIENT_OBJECTS:.o=.d))
