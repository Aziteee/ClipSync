#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <stdint.h>
#include "protocol.h"
#include "clip_bridge_client.h"
#include "ws_server.h"
#include "mdns_publish.h"
#include "protocol_json.h"
#include "daemon_config.h"
#include "last_clip.h"
#include "device_identity.h"

#define MAX_IDLE_POLL_MS 5000
#define MDNS_ANNOUNCE_INTERVAL_MS 30000LL

static volatile sig_atomic_t running = 1;
static clipsync_last_clip g_last_clip;
static int g_bridge_healthy = 0;

static long long monotonic_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static int min_timeout_until(long long deadline_ms, long long now_ms, int current_timeout_ms) {
    long long remaining;
    if (deadline_ms <= 0) return current_timeout_ms;
    if (now_ms >= deadline_ms) return 0;
    remaining = deadline_ms - now_ms;
    return remaining < current_timeout_ms ? (int)remaining : current_timeout_ms;
}

static void wake_main_loop(void *arg) {
    (void)arg;
    ws_server_wakeup();
}

static void update_module_status(clipsync_daemon_config *cfg) {
    const char *run_state;
    const char *bridge_state;
    const char *pc_state;
    char desc[256];
    static char last_desc[256] = {0};

    if (!cfg || !running) {
        run_state = "\xe2\x8f\xb9 Stopped";
        bridge_state = "-";
        pc_state = "-";
    } else {
        run_state = "\xe2\x96\xb6 Running";
        bridge_state = g_bridge_healthy ? "\xe2\x9c\x85 OK" : "\xe2\x9d\x8c ERR";
        pc_state = ws_server_authenticated_count() > 0 ? "\xe2\x9c\x85 Connected" : "\xe2\x9a\xaa Waiting";
    }

    snprintf(desc, sizeof(desc),
        "%s | Bridge: %s | PC: %s | Port: %d",
        run_state, bridge_state, pc_state, cfg ? cfg->port : 0);

    if (strcmp(desc, last_desc) == 0) return;
    strncpy(last_desc, desc, sizeof(last_desc) - 1);

    {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[clipsyncd] fork failed for module status update\n");
            return;
        }
        if (pid == 0) {
            if (fork() > 0) _exit(0);
            setenv("KSU_MODULE", "clipsyncd", 1);
            execlp("ksud", "ksud", "module", "config", "set",
                   "override.description", desc, (char *)NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }
}

static void on_clip_change(const char *text) {
    size_t len;
    if (!text) return;
    len = strlen(text);
    if (clipsync_last_clip_same(&g_last_clip, text, len)) return;
    if (clipsync_last_clip_update(&g_last_clip, text, len) != 0) {
        fprintf(stderr, "[clipsyncd] failed to update clipboard dedupe state\n");
    }

    unsigned long long ts = (unsigned long long)time(NULL) * 1000ULL;
    char *json = protocol_json_build_clipboard_push(text, ts);
    if (json) {
        ws_server_broadcast(json);
        free(json);
    }
    printf("[clipsyncd] pushed: %lu chars\n", (unsigned long)strlen(text));
}

static void on_ws_set(const char *text) {
    size_t len;
    if (!text) return;
    len = strlen(text);
    printf("[clipsyncd] received set: %lu chars\n", (unsigned long)len);
    if (clipsync_last_clip_update(&g_last_clip, text, len) != 0) {
        fprintf(stderr, "[clipsyncd] failed to update clipboard dedupe state\n");
    }
    if (clip_bridge_set_text(text) != 0) {
        fprintf(stderr, "[clipsyncd] failed to write Android clipboard\n");
    }
}

static void handle_clipboard_event(void) {
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
    long long next_mdns_announce_ms;
    int last_pc_connected = -1;
    char mdns_instance[64];

    clipsync_last_clip_init(&g_last_clip);
    clipsync_config_init(&cfg);
    if (clipsync_config_load_from_args(&cfg, argc, argv) != 0) {
        return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    printf("[clipsyncd] config: path=%s loaded=%s port=%d secret=%s\n",
           cfg.config_path,
           cfg.config_loaded ? "yes" : "no",
           cfg.port,
           cfg.secret[0] ? "(set)" : "(empty)");
    printf("[clipsyncd] starting on port %d...\n", cfg.port);

    /* Initialize subsystems */
    if (ws_server_init(cfg.port, cfg.secret) != 0) {
        fprintf(stderr, "[clipsyncd] ws_server_init failed\n");
        return 1;
    }
    ws_server_set_on_set(on_ws_set);

    if (clipsync_device_instance_name(mdns_instance, sizeof(mdns_instance)) != 0) {
        snprintf(mdns_instance, sizeof(mdns_instance), "ClipSync-Android");
    }

    if (mdns_publish_init(ws_server_mgr(), cfg.port, mdns_instance) != 0) {
        fprintf(stderr, "[clipsyncd] mdns_publish_init failed\n");
        return 1;
    }
    mdns_publish_announce();

    if (clip_bridge_init() != 0) {
        fprintf(stderr, "[clipsyncd] clip_bridge_init failed\n");
        return 1;
    }
    g_bridge_healthy = 1;
    if (clip_bridge_watch_start(wake_main_loop, NULL) != 0) {
        fprintf(stderr, "[clipsyncd] clipboard WATCH failed to start\n");
        return 1;
    }

    /* Verify bridge: read current clipboard */
    {
        char *txt = clip_bridge_get_text();
        printf("[clipsyncd] current clipboard: %s\n", txt ? txt : "(empty)");
        if (txt) free(txt);
    }

    printf("[clipsyncd] running. Waiting for connections and clipboard events...\n");
    update_module_status(&cfg);
    last_pc_connected = ws_server_authenticated_count() > 0 ? 1 : 0;
    next_mdns_announce_ms = monotonic_millis() + MDNS_ANNOUNCE_INTERVAL_MS;

    /* Event loop */
    while (running) {
        size_t authenticated_count;
        int pc_connected;
        int timeout_ms;
        long long now_ms;

        if (clip_bridge_watch_take_changed()) {
            handle_clipboard_event();
        }

        now_ms = monotonic_millis();
        timeout_ms = ws_server_next_timeout_ms(MAX_IDLE_POLL_MS);
        if (last_pc_connected == 0) {
            timeout_ms = min_timeout_until(next_mdns_announce_ms, now_ms, timeout_ms);
        }

        ws_server_poll(timeout_ms);

        if (clip_bridge_watch_take_changed()) {
            handle_clipboard_event();
        }

        authenticated_count = ws_server_authenticated_count();
        pc_connected = authenticated_count > 0 ? 1 : 0;
        if (pc_connected != last_pc_connected) {
            last_pc_connected = pc_connected;
            update_module_status(&cfg);
            if (!pc_connected) {
                mdns_publish_announce();
                next_mdns_announce_ms = monotonic_millis() + MDNS_ANNOUNCE_INTERVAL_MS;
            }
        }

        now_ms = monotonic_millis();
        if (!pc_connected && now_ms >= next_mdns_announce_ms) {
            mdns_publish_announce();
            next_mdns_announce_ms = now_ms + MDNS_ANNOUNCE_INTERVAL_MS;
        }
    }

    printf("[clipsyncd] shutting down.\n");
    clip_bridge_watch_stop();
    update_module_status(NULL);
    clipsync_last_clip_free(&g_last_clip);
    return 0;
}
