#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "protocol.h"
#include "clip_bridge_client.h"
#include "ws_server.h"
#include "mdns_publish.h"
#include "protocol_json.h"
#include "clipboard_poll.h"
#include "daemon_config.h"

static volatile int running = 1;
static char g_last_text[65536] = {0};
static int g_bridge_healthy = 0;

static void update_module_status(clipsync_daemon_config *cfg) {
    const char *run_state;
    const char *bridge_state;
    const char *pc_state;
    char cmd[512];
    static char last_cmd[512] = {0};

    if (!cfg || !running) {
        run_state = "\xe2\x8f\xb9 Stopped";
        bridge_state = "-";
        pc_state = "-";
    } else {
        run_state = "\xe2\x96\xb6 Running";
        bridge_state = g_bridge_healthy ? "\xe2\x9c\x85 OK" : "\xe2\x9d\x8c ERR";
        pc_state = ws_server_authenticated_count() > 0 ? "\xe2\x9c\x85 Connected" : "\xe2\x9a\xaa Waiting";
    }

    snprintf(cmd, sizeof(cmd),
        "ksud module config set override.description \"%s | Bridge: %s | PC: %s | Port: %d\"",
        run_state, bridge_state, pc_state, cfg ? cfg->port : 0);

    if (strcmp(cmd, last_cmd) == 0) return;
    strncpy(last_cmd, cmd, sizeof(last_cmd) - 1);

    if (system(cmd) != 0) {
        fprintf(stderr, "[clipsyncd] failed to update module description\n");
    }
}

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
    if (clip_bridge_set_text(text) != 0) {
        fprintf(stderr, "[clipsyncd] failed to write Android clipboard\n");
    }
}

static void poll_clipboard_change(void) {
    char *text = clip_bridge_get_text();
    if (!text) return;
    g_bridge_healthy = 1;
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

    if (clip_bridge_init() != 0) {
        fprintf(stderr, "[clipsyncd] clip_bridge_init failed\n");
        return 1;
    }
    g_bridge_healthy = 1;

    /* Verify bridge: read current clipboard */
    {
        char *txt = clip_bridge_get_text();
        printf("[clipsyncd] current clipboard: %s\n", txt ? txt : "(empty)");
        if (txt) free(txt);
    }

    printf("[clipsyncd] running. Waiting for connections and clipboard events...\n");
    update_module_status(&cfg);

    /* Event loop */
    int clipboard_poll_ticks = 0;
    int status_update_ticks = 0;
    while (running) {
        ws_server_poll(50);
        if (++status_update_ticks >= 100) {
            status_update_ticks = 0;
            update_module_status(&cfg);
        }
        int poll_every_ticks = clipsync_clipboard_poll_ticks_for_clients(ws_server_authenticated_count(), cfg.debounce_ms);
        if (++clipboard_poll_ticks >= poll_every_ticks) {
            clipboard_poll_ticks = 0;
            poll_clipboard_change();
        }
    }

    printf("[clipsyncd] shutting down.\n");
    update_module_status(NULL);
    return 0;
}
