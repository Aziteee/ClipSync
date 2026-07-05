#include "daemon_config.h"
#include "protocol.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static void strip_comment(char *s) {
    int in_quote = 0;
    int escaped = 0;
    for (; *s; s++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (*s == '\\' && in_quote) {
            escaped = 1;
            continue;
        }
        if (*s == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (*s == '#' && !in_quote) {
            *s = '\0';
            return;
        }
    }
}

static int copy_value(char *dst, size_t dst_len, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_len) {
        fprintf(stderr, "[config] value too long\n");
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static int parse_int_value(const char *value, int min, int max, int *out) {
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value) return -1;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0' || parsed < min || parsed > max) return -1;
    *out = (int)parsed;
    return 0;
}

static int parse_string_value(const char *value, char *out, size_t out_len) {
    char buf[256];
    size_t j = 0;

    if (*value != '"') {
        return copy_value(out, out_len, value);
    }

    value++;
    while (*value && *value != '"') {
        char ch = *value++;
        if (ch == '\\') {
            ch = *value++;
            if (ch == 'n') ch = '\n';
            else if (ch == 't') ch = '\t';
            else if (ch == '\0') return -1;
        }
        if (j + 1 >= sizeof(buf)) {
            fprintf(stderr, "[config] string value too long\n");
            return -1;
        }
        buf[j++] = ch;
    }
    if (*value != '"') return -1;
    value++;
    while (*value && isspace((unsigned char)*value)) value++;
    if (*value != '\0') return -1;

    buf[j] = '\0';
    return copy_value(out, out_len, buf);
}

static int apply_pair(clipsync_daemon_config *cfg, const char *section, const char *key, const char *value) {
    if (strcmp(section, "connection") == 0 && strcmp(key, "port") == 0) {
        if (parse_int_value(value, 1, 65535, &cfg->port) != 0) {
            fprintf(stderr, "[config] invalid connection.port: %s\n", value);
            return -1;
        }
        return 0;
    }

    if (strcmp(section, "auth") == 0 && strcmp(key, "secret") == 0) {
        return parse_string_value(value, cfg->secret, sizeof(cfg->secret));
    }

    return 0;
}

void clipsync_config_init(clipsync_daemon_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = WS_PORT;
    cfg->secret[0] = '\0';
    copy_value(cfg->config_path, sizeof(cfg->config_path), CLIPSYNC_DEFAULT_CONFIG_PATH);
}

int clipsync_config_load_file(clipsync_daemon_config *cfg, const char *path) {
    FILE *fp;
    char line[512];
    char section[64] = "";
    unsigned int line_no = 0;

    if (!cfg || !path || path[0] == '\0') return -1;
    fp = fopen(path, "rb");
    if (!fp) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "[config] failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *s;
        char *eq;
        char *key;
        char *value;
        line_no++;

        line[strcspn(line, "\r\n")] = '\0';
        strip_comment(line);
        s = trim(line);
        if (*s == '\0') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                fprintf(stderr, "[config] invalid section at %s:%u\n", path, line_no);
                fclose(fp);
                return -1;
            }
            *end = '\0';
            if (copy_value(section, sizeof(section), trim(s + 1)) != 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "[config] invalid line at %s:%u\n", path, line_no);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        key = trim(s);
        value = trim(eq + 1);
        if (apply_pair(cfg, section, key, value) != 0) {
            fprintf(stderr, "[config] failed to parse %s:%u\n", path, line_no);
            fclose(fp);
            return -1;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "[config] failed to read %s\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    cfg->config_loaded = 1;
    copy_value(cfg->config_path, sizeof(cfg->config_path), path);
    return 0;
}

int clipsync_config_load_from_args(clipsync_daemon_config *cfg, int argc, char *argv[]) {
    const char *path;
    int i;

    if (!cfg) return -1;
    path = cfg->config_path[0] ? cfg->config_path : CLIPSYNC_DEFAULT_CONFIG_PATH;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            path = argv[++i];
        }
    }

    if (clipsync_config_load_file(cfg, path) != 0) {
        return -1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_int_value(argv[++i], 1, 65535, &cfg->port) != 0) {
                fprintf(stderr, "[config] invalid --port: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            if (copy_value(cfg->secret, sizeof(cfg->secret), argv[++i]) != 0) {
                return -1;
            }
        }
    }

    return 0;
}
