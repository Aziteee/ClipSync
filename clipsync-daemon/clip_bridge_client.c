/* ClipSync clipboard access via Unix socket to Zygisk bridge
 *
 * The Zygisk module creates an abstract Unix socket @clipbridge.
 * Protocol: send "READ\n", "WRITE text\n", "HAS\n"; receive "text\n", "OK\n", "1\n"/"0\n".
 */

#include "clip_bridge_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCK_ABSTRACT_NAME "clipbridge"

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
    char *resp = sock_command("READ\n");
    if (!resp) return NULL;
    size_t len = strlen(resp);
    if (len > 0 && resp[len-1] == '\n') resp[len-1] = 0;
    if (resp[0] == 0) { free(resp); return NULL; }
    return resp;
}

int clip_bridge_set_text(const char *text) {
    char cmd[65536 + 16];
    snprintf(cmd, sizeof(cmd), "WRITE %s\n", text ? text : "");
    char *resp = sock_command(cmd);
    if (!resp) return -1;
    int ok = (strncmp(resp, "OK", 2) == 0) ? 0 : -1;
    free(resp);
    return ok;
}
