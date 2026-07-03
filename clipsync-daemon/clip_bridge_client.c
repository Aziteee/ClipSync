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
