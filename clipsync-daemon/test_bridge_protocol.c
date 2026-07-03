#include "bridge_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    _Exit(1);
}

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s failed: got %d want %d\n", name, got, want);
        _Exit(1);
    }
}

static void expect_size(const char *name, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "%s failed: got %lu want %lu\n", name, (unsigned long)got, (unsigned long)want);
        _Exit(1);
    }
}

static void expect_mem(const char *name, const char *got, const char *want, size_t len) {
    if (memcmp(got, want, len) != 0) {
        fprintf(stderr, "%s failed\n", name);
        _Exit(1);
    }
}

static void test_parse_headers(void) {
    size_t len = 0;
    expect_int("parse data", bridge_parse_len_header("DATA 12\n", "DATA ", &len), 0);
    expect_size("data len", len, 12);
    expect_int("parse write", bridge_parse_len_header("WRITE 0\n", "WRITE ", &len), 0);
    expect_size("write len", len, 0);
    expect_int("reject non numeric", bridge_parse_len_header("WRITE nope\n", "WRITE ", &len), -1);
    expect_int("reject missing newline", bridge_parse_len_header("WRITE 1", "WRITE ", &len), -1);
    expect_int("reject too large", bridge_parse_len_header("WRITE 262145\n", "WRITE ", &len), -1);
}

static void test_multiline_body(void) {
    int fds[2];
    char line[64];
    const char *body = "line 1\nline 2\n";
    char out[32];
    size_t len = 0;

    if (pipe(fds) != 0) die("pipe failed");
    if (bridge_write_cstr(fds[1], "DATA 14\n") != 0 ||
        bridge_write_full(fds[1], body, strlen(body)) != 0) {
        die("write failed");
    }
    close(fds[1]);

    expect_int("read line", bridge_read_line(fds[0], line, sizeof(line)), 0);
    expect_int("parse multiline header", bridge_parse_len_header(line, "DATA ", &len), 0);
    expect_size("multiline len", len, strlen(body));
    expect_int("read multiline body", bridge_read_full(fds[0], out, len), 0);
    expect_mem("multiline body", out, body, len);
    close(fds[0]);
}

static void test_large_body(void) {
    int fds[2];
    char line[64];
    char *body;
    char *out;
    size_t len = 70000;
    size_t parsed = 0;
    pid_t pid;

    body = (char *)malloc(len);
    out = (char *)malloc(len);
    if (!body || !out) die("malloc failed");
    memset(body, 'x', len);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) die("socketpair failed");
    pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        close(fds[0]);
        if (bridge_write_cstr(fds[1], "DATA 70000\n") != 0 ||
            bridge_write_full(fds[1], body, len) != 0) {
            _Exit(1);
        }
        close(fds[1]);
        _Exit(0);
    }
    close(fds[1]);

    expect_int("read large line", bridge_read_line(fds[0], line, sizeof(line)), 0);
    expect_int("parse large header", bridge_parse_len_header(line, "DATA ", &parsed), 0);
    expect_size("large parsed len", parsed, len);
    expect_int("read large body", bridge_read_full(fds[0], out, len), 0);
    expect_mem("large body", out, body, len);

    close(fds[0]);
    if (waitpid(pid, NULL, 0) < 0) die("waitpid failed");
    free(out);
    free(body);
}

static void test_incomplete_body(void) {
    int fds[2];
    char out[8];

    if (pipe(fds) != 0) die("pipe failed");
    if (bridge_write_cstr(fds[1], "abc") != 0) die("short write failed");
    close(fds[1]);
    expect_int("incomplete body", bridge_read_full(fds[0], out, sizeof(out)), -1);
    close(fds[0]);
}

int main(void) {
    test_parse_headers();
    test_multiline_body();
    test_large_body();
    test_incomplete_body();
    printf("bridge protocol tests passed\n");
    return 0;
}
