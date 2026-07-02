#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "protocol.h"
#include "binder_clip.h"
#include "ws_server.h"
#include "mdns_publish.h"
#include "protocol_json.h"
#include "clipboard_poll.h"
#include "daemon_config.h"

static volatile int running = 1;
static char g_last_text[65536] = {0};

static void on_clip_change(const char *text) {
    if (!text) return;
    /* Dedup */
    if (strcmp(text, g_last_text) == 0) return;
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);

    unsigned long long ts = (unsigned long long)time(NULL) * 1000ULL;
    char *json = protocol_json_build_clipboard_push(text, ts);
    if (json) {
        ws_server_broadcast(json);
        free(json);
    }
    printf("[clipsyncd] pushed: %lu chars\n", (unsigned long)strlen(text));
}

static void on_ws_set(const char *text) {
    if (!text) return;
    printf("[clipsyncd] received set: %lu chars\n", (unsigned long)strlen(text));
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);
    if (binder_clip_set_text(text) != 0) {
        fprintf(stderr, "[clipsyncd] failed to write Android clipboard\n");
    }
}

static void poll_clipboard_change(void) {
    char *text = binder_clip_get_text();
    if (!text) return;
    on_clip_change(text);
    free(text);
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    clipsync_daemon_config cfg;
    clipsync_config_init(&cfg);
    if (clipsync_config_load_from_args(&cfg, argc, argv) != 0) {
        return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] config: path=%s loaded=%s port=%d secret=%s debounce_ms=%d\n",
           cfg.config_path,
           cfg.config_loaded ? "yes" : "no",
           cfg.port,
           cfg.secret[0] ? "(set)" : "(empty)",
           cfg.debounce_ms);
    printf("[clipsyncd] starting on port %d...\n", cfg.port);

    /* Initialize subsystems */
    if (ws_server_init(cfg.port, cfg.secret) != 0) {
        fprintf(stderr, "[clipsyncd] ws_server_init failed\n");
        return 1;
    }
    ws_server_set_on_set(on_ws_set);

    if (mdns_publish_init(ws_server_mgr(), cfg.port, "ClipSync Android") != 0) {
        fprintf(stderr, "[clipsyncd] mdns_publish_init failed\n");
        return 1;
    }

    if (binder_clip_init() != 0) {
        fprintf(stderr, "[clipsyncd] binder_clip_init failed\n");
        return 1;
    }
    binder_clip_set_callback(on_clip_change);

    /* Verify bridge: read current clipboard */
    {
        char *txt = binder_clip_get_text();
        printf("[clipsyncd] current clipboard: %s\n", txt ? txt : "(empty)");
        if (txt) free(txt);
    }

    printf("[clipsyncd] running. Waiting for connections and clipboard events...\n");

    /* Event loop */
    int clipboard_poll_ticks = 0;
    while (running) {
        ws_server_poll(50);   /* drive mongoose (WS + mDNS) */
        int poll_every_ticks = clipsync_clipboard_poll_ticks_for_clients(ws_server_authenticated_count());
        if (++clipboard_poll_ticks >= poll_every_ticks) {
            clipboard_poll_ticks = 0;
            poll_clipboard_change();
        }
    }

    printf("[clipsyncd] shutting down.\n");
    return 0;
}
