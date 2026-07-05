#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include "bridge_protocol.h"

#define SOCK_ABSTRACT_NAME "clipbridge"

static int connect_bridge(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCK_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);
    socklen_t len = (socklen_t)(sizeof(sa_family_t) + 1 + strlen(SOCK_ABSTRACT_NAME));

    if (connect(fd, (struct sockaddr *)&addr, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    int fd;
    if (argc < 2) {
        fprintf(stderr, "usage: test_bridge_client read|write [text]|has|watch\n");
        return 2;
    }

    fd = connect_bridge();
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    if (strcmp(argv[1], "read") == 0) {
        if (bridge_write_cstr(fd, "READ\n") != 0) {
            perror("write");
            close(fd);
            return 1;
        }
    } else if (strcmp(argv[1], "write") == 0) {
        const char *text = argc >= 3 ? argv[2] : "";
        char header[64];
        snprintf(header, sizeof(header), "WRITE %lu\n", (unsigned long)strlen(text));
        if (bridge_write_cstr(fd, header) != 0 ||
            bridge_write_full(fd, text, strlen(text)) != 0) {
            perror("write");
            close(fd);
            return 1;
        }
    } else if (strcmp(argv[1], "has") == 0) {
        if (bridge_write_cstr(fd, "HAS\n") != 0) {
            perror("write");
            close(fd);
            return 1;
        }
    } else if (strcmp(argv[1], "watch") == 0) {
        if (bridge_write_cstr(fd, "WATCH\n") != 0) {
            perror("write");
            close(fd);
            return 1;
        }
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        close(fd);
        return 2;
    }

    char line[256];
    if (bridge_read_line(fd, line, sizeof(line)) != 0) {
        fprintf(stderr, "failed to read response header\n");
        close(fd);
        return 1;
    }
    if (strncmp(line, "DATA ", 5) == 0) {
        size_t len = 0;
        char *body;
        if (bridge_parse_len_header(line, "DATA ", &len) != 0) {
            fprintf(stderr, "bad DATA header: %s", line);
            close(fd);
            return 1;
        }
        body = (char *)malloc(len + 1);
        if (!body) {
            close(fd);
            return 1;
        }
        if (bridge_read_full(fd, body, len) != 0) {
            fprintf(stderr, "failed to read DATA body\n");
            free(body);
            close(fd);
            return 1;
        }
        body[len] = '\0';
        printf("%s", body);
        free(body);
    } else {
        printf("%s", line);
        fflush(stdout);
    }
    while (strcmp(argv[1], "watch") == 0 && bridge_read_line(fd, line, sizeof(line)) == 0) {
        printf("%s", line);
        fflush(stdout);
    }
    close(fd);
    return 0;
}
