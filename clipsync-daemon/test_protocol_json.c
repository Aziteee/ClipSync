#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol_json.h"

static int fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    return 1;
}

int main(void) {
    int failures = 0;

    const char *set = "{\"type\":\"clipboard_set\",\"text\":\"hello\"}";
    if (protocol_json_type(set, strlen(set)) != CLIPSYNC_MSG_CLIPBOARD_SET) {
        failures += fail("clipboard_set type parse failed");
    }

    char *text = protocol_json_get_text(set, strlen(set));
    if (!text || strcmp(text, "hello") != 0) {
        failures += fail("text parse failed");
    }
    free(text);

    const char *auth = "{\"type\":\"auth\",\"response\":\"abc123\"}";
    char *response = protocol_json_get_auth_response(auth, strlen(auth));
    if (!response || strcmp(response, "abc123") != 0) {
        failures += fail("auth response parse failed");
    }
    free(response);

    char *escaped = protocol_json_escape("quote:\" slash:\\ line:\n");
    if (!escaped || strcmp(escaped, "quote:\\\" slash:\\\\ line:\\n") != 0) {
        failures += fail("json escape failed");
    }
    free(escaped);

    char *push = protocol_json_build_clipboard_push("hi \"pc\"", 1234);
    if (!push || strcmp(push, "{\"type\":\"clipboard_push\",\"text\":\"hi \\\"pc\\\"\",\"ts\":1234}") != 0) {
        failures += fail("clipboard_push build failed");
    }
    free(push);

    char *hello = protocol_json_build_hello("001122");
    if (!hello || strcmp(hello, "{\"type\":\"hello\",\"challenge\":\"001122\"}") != 0) {
        failures += fail("hello build failed");
    }
    free(hello);

    if (failures == 0) {
        printf("protocol_json tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
