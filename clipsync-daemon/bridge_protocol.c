#include "bridge_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int bridge_read_line(int fd, char *buf, size_t buf_len) {
    size_t pos = 0;

    if (!buf || buf_len == 0) return -1;
    while (pos + 1 < buf_len) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[pos++] = ch;
        if (ch == '\n') break;
    }
    buf[pos] = '\0';
    if (pos == 0) return -1;
    if (buf[pos - 1] != '\n') return -1;
    return 0;
}

int bridge_read_full(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int bridge_write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int bridge_write_cstr(int fd, const char *text) {
    const char *safe = text ? text : "";
    return bridge_write_full(fd, safe, strlen(safe));
}

int bridge_parse_len_header(const char *line, const char *prefix, size_t *out_len) {
    unsigned long parsed = 0;
    size_t prefix_len;
    const char *p;

    if (!line || !prefix || !out_len) return -1;
    prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) return -1;
    p = line + prefix_len;
    if (*p < '0' || *p > '9') return -1;

    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');
        if (parsed > (CLIPSYNC_BRIDGE_MAX_PAYLOAD - digit) / 10U) return -1;
        parsed = parsed * 10U + digit;
        p++;
    }
    if (*p != '\n' || parsed > CLIPSYNC_BRIDGE_MAX_PAYLOAD) return -1;
    *out_len = (size_t)parsed;
    return 0;
}

clipsync_watch_line bridge_parse_watch_line(const char *line) {
    if (!line) return CLIPSYNC_WATCH_LINE_UNKNOWN;
    if (strcmp(line, "READY\n") == 0) return CLIPSYNC_WATCH_LINE_READY;
    if (strcmp(line, "CHANGED\n") == 0) return CLIPSYNC_WATCH_LINE_CHANGED;
    if (strncmp(line, "ACTION ", 7) == 0) return CLIPSYNC_WATCH_LINE_ACTION;
    return CLIPSYNC_WATCH_LINE_UNKNOWN;
}

int bridge_parse_action_line(const char *line, int *out_action_id) {
    const char *p;
    int value = 0;
    if (!line || !out_action_id) return -1;
    if (strncmp(line, "ACTION ", 7) != 0) return -1;
    p = line + 7;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    if (*p != '\n') return -1;
    *out_action_id = value;
    return 0;
}
