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

static volatile int running = 1;
static const char *g_secret = NULL;
static char g_last_text[65536] = {0};

static void on_clip_change(const char *text) {
    if (!text) return;
    /* Dedup */
    if (strcmp(text, g_last_text) == 0) return;
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);

    /* Build push JSON */
    char json[65536 * 2];
    unsigned long ts = (unsigned long)time(NULL) * 1000;
    snprintf(json, sizeof(json),
        "{\"type\":\"clipboard_push\",\"text\":\"%s\",\"ts\":%lu}",
        text, ts);
    ws_server_broadcast(json);
    printf("[clipsyncd] pushed: %lu chars\n", (unsigned long)strlen(text));
}

static void on_ws_set(const char *text) {
    if (!text) return;
    printf("[clipsyncd] received set: %lu chars\n", (unsigned long)strlen(text));
    strncpy(g_last_text, text, sizeof(g_last_text) - 1);
    binder_clip_set_text(text);
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    int port = WS_PORT;
    g_secret = "";

    /* Parse args: clipsyncd [--port N] [--secret S] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            g_secret = argv[++i];
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] starting on port %d...\n", port);

    /* Initialize subsystems */
    if (ws_server_init(port, g_secret) != 0) {
        fprintf(stderr, "[clipsyncd] ws_server_init failed\n");
        return 1;
    }
    ws_server_set_on_set(on_ws_set);

    if (mdns_publish_init(ws_server_mgr(), port, "ClipSync Android") != 0) {
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
    while (running) {
        ws_server_poll(50);   /* drive mongoose (WS + mDNS) */
    }

    printf("[clipsyncd] shutting down.\n");
    return 0;
}
