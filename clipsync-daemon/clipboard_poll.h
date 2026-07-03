#ifndef CLIPSYNC_CLIPBOARD_POLL_H
#define CLIPSYNC_CLIPBOARD_POLL_H

#include <stddef.h>

#define CLIPBOARD_POLL_TICK_MS 50
#define CLIPBOARD_POLL_IDLE_MIN_MS 5000

int clipsync_clipboard_poll_ticks_for_clients(size_t authenticated_clients, int debounce_ms);

#endif
