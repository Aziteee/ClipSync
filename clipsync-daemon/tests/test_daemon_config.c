#include "daemon_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s failed: got %d want %d\n", name, got, want);
        _Exit(1);
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s failed: got '%s' want '%s'\n", name, got, want);
        _Exit(1);
    }
}

static void write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen");
        _Exit(1);
    }
    if (fputs(content, fp) < 0) {
        perror("fputs");
        fclose(fp);
        _Exit(1);
    }
    fclose(fp);
}

int main(void) {
    const char *path = "build/test_clipsync.toml";
    clipsync_daemon_config cfg;

    clipsync_config_init(&cfg);
    expect_int("default port", cfg.port, 5287);
    expect_str("default secret", cfg.secret, "");

    if (clipsync_config_load_file(&cfg, "build/no-such-clipsync.toml") != 0) {
        fprintf(stderr, "missing config should keep defaults without failing\n");
        return 1;
    }
    expect_int("missing file keeps default port", cfg.port, 5287);

    write_file(path,
               "# Android daemon config\n"
               "[connection]\n"
               "port = 6123\n"
               "\n"
               "[auth]\n"
               "secret = \"phone secret\"\n");

    clipsync_config_init(&cfg);
    if (clipsync_config_load_file(&cfg, path) != 0) {
        fprintf(stderr, "config file load failed\n");
        return 1;
    }
    expect_int("configured port", cfg.port, 6123);
    expect_str("configured secret", cfg.secret, "phone secret");

    char *argv[] = {
        "clipsyncd",
        "--config", (char *)path,
        "--port", "7001",
        "--secret", "cli secret",
    };
    clipsync_config_init(&cfg);
    if (clipsync_config_load_from_args(&cfg, 7, argv) != 0) {
        fprintf(stderr, "config args load failed\n");
        return 1;
    }
    expect_int("cli port overrides file", cfg.port, 7001);
    expect_str("cli secret overrides file", cfg.secret, "cli secret");

    remove(path);
    printf("daemon config tests passed\n");
    return 0;
}
