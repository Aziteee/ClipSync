#include "clipboard_poll.h"
#include <stdio.h>
#include <stdlib.h>

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s failed: got %d want %d\n", name, got, want);
        _Exit(1);
    }
}

int main(void) {
    expect_int("idle poll ticks", clipsync_clipboard_poll_ticks_for_clients(0), 100);
    expect_int("connected poll ticks", clipsync_clipboard_poll_ticks_for_clients(1), 10);
    expect_int("multi-client poll ticks", clipsync_clipboard_poll_ticks_for_clients(4), 10);
    printf("clipboard poll cadence tests passed\n");
    return 0;
}
