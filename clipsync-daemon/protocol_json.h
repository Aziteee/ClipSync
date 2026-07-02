#ifndef CLIPSYNC_PROTOCOL_JSON_H
#define CLIPSYNC_PROTOCOL_JSON_H

#include <stddef.h>

typedef enum {
    CLIPSYNC_MSG_UNKNOWN = 0,
    CLIPSYNC_MSG_CLIPBOARD_SET,
    CLIPSYNC_MSG_PING,
    CLIPSYNC_MSG_PONG,
    CLIPSYNC_MSG_AUTH
} clipsync_msg_type;

clipsync_msg_type protocol_json_type(const char *json, size_t len);
char *protocol_json_get_text(const char *json, size_t len);
char *protocol_json_get_auth_response(const char *json, size_t len);
char *protocol_json_build_hello(const char *challenge_hex);
char *protocol_json_build_auth_ok(void);
char *protocol_json_build_auth_fail(void);
char *protocol_json_build_pong(void);
char *protocol_json_build_clipboard_push(const char *text, unsigned long long ts);
char *protocol_json_escape(const char *text);

#endif
