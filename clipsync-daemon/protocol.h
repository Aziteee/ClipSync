#ifndef CLIPSYNC_PROTOCOL_H
#define CLIPSYNC_PROTOCOL_H

#define WS_PORT 5287
#define DEFAULT_DEBOUNCE_MS 300
#define MDNS_SERVICE_TYPE "_clipsync._tcp"

/* Message type tags */
#define MSG_TYPE_PUSH     "clipboard_push"
#define MSG_TYPE_SET      "clipboard_set"
#define MSG_TYPE_PING     "ping"
#define MSG_TYPE_PONG     "pong"
#define MSG_TYPE_HELLO    "hello"
#define MSG_TYPE_AUTH     "auth"
#define MSG_TYPE_AUTH_OK  "auth_ok"
#define MSG_TYPE_AUTH_FAIL "auth_fail"

#endif
