#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "protocol.h"

static volatile int running = 1;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] starting on port %d...\n", WS_PORT);

    while (running) {
        sleep(1);
    }

    printf("[clipsyncd] shutting down.\n");
    return 0;
}
