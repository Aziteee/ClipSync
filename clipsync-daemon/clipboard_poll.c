#include "clipboard_poll.h"

int clipsync_clipboard_poll_ticks_for_clients(size_t authenticated_clients) {
    return authenticated_clients > 0 ? CLIPBOARD_POLL_TICKS_CONNECTED : CLIPBOARD_POLL_TICKS_IDLE;
}
