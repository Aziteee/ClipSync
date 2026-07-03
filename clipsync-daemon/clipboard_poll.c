#include "clipboard_poll.h"

static int ticks_for_ms(int ms) {
    int ticks;
    if (ms <= 0) ms = CLIPBOARD_POLL_TICK_MS;
    ticks = (ms + CLIPBOARD_POLL_TICK_MS - 1) / CLIPBOARD_POLL_TICK_MS;
    return ticks > 0 ? ticks : 1;
}

int clipsync_clipboard_poll_ticks_for_clients(size_t authenticated_clients, int debounce_ms) {
    int interval_ms = debounce_ms;
    if (authenticated_clients == 0 && interval_ms < CLIPBOARD_POLL_IDLE_MIN_MS) {
        interval_ms = CLIPBOARD_POLL_IDLE_MIN_MS;
    }
    return ticks_for_ms(interval_ms);
}
