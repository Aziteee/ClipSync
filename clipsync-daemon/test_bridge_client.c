#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

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
    char cmd[65536 + 16];
    if (argc < 2) {
        fprintf(stderr, "usage: test_bridge_client read|write [text]\n");
        return 2;
    }

    if (strcmp(argv[1], "read") == 0) {
        snprintf(cmd, sizeof(cmd), "READ\n");
    } else if (strcmp(argv[1], "write") == 0) {
        snprintf(cmd, sizeof(cmd), "WRITE %s\n", argc >= 3 ? argv[2] : "");
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        return 2;
    }

    int fd = connect_bridge();
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        perror("read");
        return 1;
    }
    buf[n] = '\0';
    printf("%s", buf);
    return 0;
}
