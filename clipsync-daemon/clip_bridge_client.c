/* ClipSync clipboard access via Unix socket to Zygisk bridge
 *
 * The Zygisk module creates an abstract Unix socket @clipbridge.
 * Protocol: send "READ\n", "WRITE <len>\n<body>", "HAS\n".
 */

#include "clip_bridge_client.h"
#include "bridge_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <stdatomic.h>

#define SOCK_ABSTRACT_NAME "clipbridge"

static atomic_int g_watch_running = 0;
static atomic_int g_watch_changed = 0;
static atomic_int g_watch_fd = -1;
static pthread_t g_watch_thread;
static void (*g_watch_notify_fn)(void *) = NULL;
static void *g_watch_notify_arg = NULL;

static int sock_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCK_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addr_len = (socklen_t)(1 + strlen(SOCK_ABSTRACT_NAME) + sizeof(sa_family_t));
    if (connect(fd, (struct sockaddr*)&addr, addr_len) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int clip_bridge_init(void) {
    /* Retry with backoff: system_server may not be ready yet at boot. */
    for (int attempt = 0; attempt < 10; attempt++) {
        int fd = sock_connect();
        if (fd >= 0) {
            close(fd);
            printf("[clip_bridge] connected to clipboard bridge (attempt %d)\n", attempt + 1);
            return 0;
        }
        if (attempt < 9) {
            sleep(1);
        }
    }
    fprintf(stderr, "[clip_bridge] cannot connect to @%s after 10 attempts\n", SOCK_ABSTRACT_NAME);
    return -1;
}

char *clip_bridge_get_text(void) {
    int fd = sock_connect();
    char line[256];
    size_t len = 0;
    char *text;

    if (fd < 0) return NULL;
    if (bridge_write_cstr(fd, "READ\n") != 0 ||
        bridge_read_line(fd, line, sizeof(line)) != 0 ||
        bridge_parse_len_header(line, "DATA ", &len) != 0) {
        close(fd);
        return NULL;
    }

    text = (char *)malloc(len + 1);
    if (!text) {
        close(fd);
        return NULL;
    }
    if (bridge_read_full(fd, text, len) != 0) {
        free(text);
        close(fd);
        return NULL;
    }
    close(fd);
    text[len] = '\0';
    if (len == 0) {
        free(text);
        return NULL;
    }
    return text;
}

int clip_bridge_set_text(const char *text) {
    const char *safe = text ? text : "";
    size_t len = strlen(safe);
    char header[64];
    char line[256];
    int fd;
    int ok;

    if (len > CLIPSYNC_BRIDGE_MAX_PAYLOAD) return -1;
    fd = sock_connect();
    if (fd < 0) return -1;
    snprintf(header, sizeof(header), "WRITE %lu\n", (unsigned long)len);
    if (bridge_write_cstr(fd, header) != 0 ||
        bridge_write_full(fd, safe, len) != 0 ||
        bridge_read_line(fd, line, sizeof(line)) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    ok = (strncmp(line, "OK", 2) == 0) ? 0 : -1;
    return ok;
}

static void *watch_thread_main(void *arg) {
    (void)arg;
    while (atomic_load(&g_watch_running)) {
        int fd = sock_connect();
        char line[256];
        if (fd < 0) {
            sleep(1);
            continue;
        }
        atomic_store(&g_watch_fd, fd);

        if (bridge_write_cstr(fd, "WATCH\n") != 0) {
            close(fd);
            atomic_store(&g_watch_fd, -1);
            sleep(1);
            continue;
        }

        if (bridge_read_line(fd, line, sizeof(line)) != 0 ||
            bridge_parse_watch_line(line) != CLIPSYNC_WATCH_LINE_READY) {
            if (strncmp(line, "ERR ", 4) == 0) {
                fprintf(stderr, "[clip_bridge] WATCH unavailable: %s", line);
            }
            close(fd);
            atomic_store(&g_watch_fd, -1);
            sleep(1);
            continue;
        }

        printf("[clip_bridge] WATCH ready\n");

        while (atomic_load(&g_watch_running)) {
            clipsync_watch_line kind;
            if (bridge_read_line(fd, line, sizeof(line)) != 0) {
                break;
            }
            kind = bridge_parse_watch_line(line);
            if (kind == CLIPSYNC_WATCH_LINE_CHANGED) {
                atomic_store(&g_watch_changed, 1);
                if (g_watch_notify_fn) {
                    g_watch_notify_fn(g_watch_notify_arg);
                }
            } else if (kind != CLIPSYNC_WATCH_LINE_READY) {
                fprintf(stderr, "[clip_bridge] WATCH unknown line: %s", line);
            }
        }

        close(fd);
        atomic_store(&g_watch_fd, -1);
        if (atomic_load(&g_watch_running)) {
            sleep(1);
        }
    }
    return NULL;
}

int clip_bridge_watch_start(void (*notify_fn)(void *), void *notify_arg) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_watch_running, &expected, 1)) {
        return 0;
    }
    g_watch_notify_fn = notify_fn;
    g_watch_notify_arg = notify_arg;
    atomic_store(&g_watch_changed, 0);
    if (pthread_create(&g_watch_thread, NULL, watch_thread_main, NULL) != 0) {
        atomic_store(&g_watch_running, 0);
        g_watch_notify_fn = NULL;
        g_watch_notify_arg = NULL;
        return -1;
    }
    return 0;
}

void clip_bridge_watch_stop(void) {
    int fd;
    if (!atomic_exchange(&g_watch_running, 0)) return;
    fd = atomic_load(&g_watch_fd);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    pthread_join(g_watch_thread, NULL);
    g_watch_notify_fn = NULL;
    g_watch_notify_arg = NULL;
}

int clip_bridge_watch_take_changed(void) {
    return atomic_exchange(&g_watch_changed, 0);
}
