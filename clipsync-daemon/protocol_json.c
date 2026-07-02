#include "protocol_json.h"

#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_literal(const char *text) {
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

static char *build_one_field(const char *type, const char *field, const char *value) {
    const char *safe_value = value ? value : "";
    int needed = snprintf(NULL, 0, "{\"type\":\"%s\",\"%s\":\"%s\"}", type, field, safe_value);
    if (needed < 0) return NULL;

    char *out = (char *)malloc((size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "{\"type\":\"%s\",\"%s\":\"%s\"}", type, field, safe_value);
    return out;
}

clipsync_msg_type protocol_json_type(const char *json, size_t len) {
    if (!json) return CLIPSYNC_MSG_UNKNOWN;

    char *type = mg_json_get_str(mg_str_n(json, len), "$.type");
    if (!type) return CLIPSYNC_MSG_UNKNOWN;

    clipsync_msg_type result = CLIPSYNC_MSG_UNKNOWN;
    if (strcmp(type, "clipboard_set") == 0) {
        result = CLIPSYNC_MSG_CLIPBOARD_SET;
    } else if (strcmp(type, "ping") == 0) {
        result = CLIPSYNC_MSG_PING;
    } else if (strcmp(type, "pong") == 0) {
        result = CLIPSYNC_MSG_PONG;
    } else if (strcmp(type, "auth") == 0) {
        result = CLIPSYNC_MSG_AUTH;
    }

    free(type);
    return result;
}

char *protocol_json_get_text(const char *json, size_t len) {
    if (!json) return NULL;
    return mg_json_get_str(mg_str_n(json, len), "$.text");
}

char *protocol_json_get_auth_response(const char *json, size_t len) {
    if (!json) return NULL;
    return mg_json_get_str(mg_str_n(json, len), "$.response");
}

char *protocol_json_build_hello(const char *challenge_hex) {
    return build_one_field("hello", "challenge", challenge_hex);
}

char *protocol_json_build_auth_ok(void) {
    return dup_literal("{\"type\":\"auth_ok\"}");
}

char *protocol_json_build_auth_fail(void) {
    return dup_literal("{\"type\":\"auth_fail\"}");
}

char *protocol_json_build_pong(void) {
    return dup_literal("{\"type\":\"pong\"}");
}

char *protocol_json_escape(const char *text) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *in = (const unsigned char *)(text ? text : "");
    size_t needed = 0;

    for (size_t i = 0; in[i] != '\0'; i++) {
        switch (in[i]) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                needed += 2;
                break;
            default:
                needed += in[i] < 0x20 ? 6 : 1;
                break;
        }
    }

    char *out = (char *)malloc(needed + 1);
    if (!out) return NULL;

    char *p = out;
    for (size_t i = 0; in[i] != '\0'; i++) {
        switch (in[i]) {
            case '"':
                *p++ = '\\';
                *p++ = '"';
                break;
            case '\\':
                *p++ = '\\';
                *p++ = '\\';
                break;
            case '\b':
                *p++ = '\\';
                *p++ = 'b';
                break;
            case '\f':
                *p++ = '\\';
                *p++ = 'f';
                break;
            case '\n':
                *p++ = '\\';
                *p++ = 'n';
                break;
            case '\r':
                *p++ = '\\';
                *p++ = 'r';
                break;
            case '\t':
                *p++ = '\\';
                *p++ = 't';
                break;
            default:
                if (in[i] < 0x20) {
                    *p++ = '\\';
                    *p++ = 'u';
                    *p++ = '0';
                    *p++ = '0';
                    *p++ = hex[in[i] >> 4];
                    *p++ = hex[in[i] & 0x0f];
                } else {
                    *p++ = (char)in[i];
                }
                break;
        }
    }
    *p = '\0';
    return out;
}

char *protocol_json_build_clipboard_push(const char *text, unsigned long long ts) {
    char *escaped = protocol_json_escape(text);
    if (!escaped) return NULL;

    int needed = snprintf(NULL, 0, "{\"type\":\"clipboard_push\",\"text\":\"%s\",\"ts\":%llu}", escaped, ts);
    if (needed < 0) {
        free(escaped);
        return NULL;
    }

    char *out = (char *)malloc((size_t)needed + 1);
    if (out) {
        snprintf(out, (size_t)needed + 1, "{\"type\":\"clipboard_push\",\"text\":\"%s\",\"ts\":%llu}", escaped, ts);
    }
    free(escaped);
    return out;
}
