#ifndef CLIPSYNC_CLIPBOARD_POLL_H
#define CLIPSYNC_CLIPBOARD_POLL_H

#include <stddef.h>

#define CLIPBOARD_POLL_TICKS_CONNECTED 10
#define CLIPBOARD_POLL_TICKS_IDLE 100

int clipsync_clipboard_poll_ticks_for_clients(size_t authenticated_clients);

#endif
