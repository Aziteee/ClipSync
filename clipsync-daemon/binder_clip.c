/* ClipSync â€?clipboard access via Unix socket to Zygisk bridge
 *
 * The Zygisk module creates a Unix socket at /data/local/tmp/clipbridge.sock
 * Protocol: send "READ\n", "WRITE text\n", "HAS\n"; receive "text\n", "OK\n", "1\n"/"0\n"
 */

#include "binder_clip.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCK_PATH "/data/local/tmp/clipbridge.sock"

static int sock_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static char *sock_command(const char *cmd) {
    int fd = sock_connect();
    if (fd < 0) return NULL;
    write(fd, cmd, strlen(cmd));
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    return strdup(buf);
}

int binder_clip_init(void) {
    int fd = sock_connect();
    if (fd < 0) {
        fprintf(stderr, "[binder_clip] cannot connect to %s (Zygisk module not loaded?)\n", SOCK_PATH);
        return -1;
    }
    close(fd);
    printf("[binder_clip] connected to clipboard bridge\n");
    return 0;
}

char *binder_clip_get_text(void) {
    char *resp = sock_command("READ\n");
    if (!resp) return NULL;
    size_t len = strlen(resp);
    if (len > 0 && resp[len-1] == '\n') resp[len-1] = 0;
    if (resp[0] == 0) { free(resp); return NULL; }
    return resp;
}

int binder_clip_set_text(const char *text) {
    char cmd[65536 + 16];
    snprintf(cmd, sizeof(cmd), "WRITE %s\n", text ? text : "");
    char *resp = sock_command(cmd);
    if (!resp) return -1;
    int ok = (strncmp(resp, "OK", 2) == 0) ? 0 : -1;
    free(resp);
    return ok;
}

void binder_clip_set_callback(clip_change_cb cb) {
    (void)cb;
    printf("[binder_clip] callback registered (no listener â€?poll mode)\n");
}
