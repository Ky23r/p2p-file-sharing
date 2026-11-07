#include "protocol.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--help]\n"
            "P2P file sharing control server (foundation).\n"
            "Default endpoint: %s:%u\n"
            "TCP server initialization will be added in the server milestone.\n",
            program, DEFAULT_SERVER_IP, DEFAULT_SERVER_PORT);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return 0;
    }
    usage(argc == 1 ? stdout : stderr, argv[0]);
    return argc == 1 ? 0 : 1;
}
