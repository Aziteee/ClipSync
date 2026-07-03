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
    expect_int("idle poll ticks", clipsync_clipboard_poll_ticks_for_clients(0, 300), 100);
    expect_int("idle respects larger debounce", clipsync_clipboard_poll_ticks_for_clients(0, 6000), 120);
    expect_int("connected poll ticks", clipsync_clipboard_poll_ticks_for_clients(1, 450), 9);
    expect_int("multi-client poll ticks", clipsync_clipboard_poll_ticks_for_clients(4, 300), 6);
    expect_int("minimum one tick", clipsync_clipboard_poll_ticks_for_clients(1, 1), 1);
    printf("clipboard poll cadence tests passed\n");
    return 0;
}
