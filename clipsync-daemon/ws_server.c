#include "ws_server.h"
#include "protocol.h"
#include "protocol_json.h"
#include "crypto_hmac.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS 1
#define CHALLENGE_BYTES 32
#define CHALLENGE_HEX_LEN 64
#define AUTH_TIMEOUT_MS 5000
#define AUTHED_IDLE_TIMEOUT_MS 30000

typedef struct {
    struct mg_connection *conn;
    int authenticated;
    unsigned long long auth_deadline_ms;
    unsigned long long last_recv_ms;
    char challenge[CHALLENGE_HEX_LEN + 1];
} ws_client;

static struct mg_mgr g_mgr;
static struct mg_connection *g_listener = NULL;
static ws_client g_clients[MAX_CLIENTS];
static char g_secret[256];
static ws_on_set_fn g_on_set = NULL;
static int g_initialized = 0;
static int g_wakeup_enabled = 0;

static ws_client *client_find(struct mg_connection *c);
static ws_client *client_alloc(struct mg_connection *c);
static void client_free(struct mg_connection *c);
static void close_expired_auth_clients(void);
static int make_challenge(char out[CHALLENGE_HEX_LEN + 1]);
static void send_text(struct mg_connection *c, const char *json);
static void handle_auth(struct mg_connection *c, ws_client *client, const char *json, size_t len);
static void handle_authed_message(struct mg_connection *c, const char *json, size_t len);
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data);

static ws_client *client_find(struct mg_connection *c) {
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].conn == c) {
            return &g_clients[i];
        }
    }
    return NULL;
}

static ws_client *client_alloc(struct mg_connection *c) {
    ws_client *existing = client_find(c);
    if (existing) return existing;

    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].conn) {
            memset(&g_clients[i], 0, sizeof(g_clients[i]));
            g_clients[i].conn = c;
            g_clients[i].auth_deadline_ms = mg_millis() + AUTH_TIMEOUT_MS;
            g_clients[i].last_recv_ms = mg_millis();
            return &g_clients[i];
        }
    }
    return NULL;
}

static void close_expired_auth_clients(void) {
    unsigned long long now = mg_millis();
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].conn) continue;
        if (!g_clients[i].authenticated) {
            if (g_clients[i].auth_deadline_ms != 0 &&
                now >= g_clients[i].auth_deadline_ms) {
                g_clients[i].conn->is_closing = 1;
            }
        } else if (g_clients[i].last_recv_ms != 0 &&
                   now - g_clients[i].last_recv_ms >= AUTHED_IDLE_TIMEOUT_MS) {
            g_clients[i].conn->is_closing = 1;
        }
    }
}

static void client_free(struct mg_connection *c) {
    ws_client *client = client_find(c);
    if (client) {
        memset(client, 0, sizeof(*client));
    }
}

static int make_challenge(char out[CHALLENGE_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char random_bytes[CHALLENGE_BYTES];

    if (!out || !mg_random(random_bytes, sizeof(random_bytes))) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        out[i * 2] = hex[random_bytes[i] >> 4];
        out[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    out[CHALLENGE_HEX_LEN] = '\0';
    return 0;
}

static void send_text(struct mg_connection *c, const char *json) {
    if (c && json) {
        mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
    }
}

static void handle_auth(struct mg_connection *c, ws_client *client, const char *json, size_t len) {
    if (protocol_json_type(json, len) != CLIPSYNC_MSG_AUTH) {
        char *fail = protocol_json_build_auth_fail();
        send_text(c, fail);
        free(fail);
        c->is_closing = 1;
        return;
    }

    char expected[SHA256_HEX_LEN + 1];
    char *response = protocol_json_get_auth_response(json, len);
    if (!response || clipsync_hmac_sha256_hex(g_secret, client->challenge, expected) != 0 ||
        strlen(response) != SHA256_HEX_LEN ||
        !clipsync_secure_hex_eq(response, expected, SHA256_HEX_LEN)) {
        free(response);
        char *fail = protocol_json_build_auth_fail();
        send_text(c, fail);
        free(fail);
        c->is_closing = 1;
        return;
    }

    free(response);
    client->authenticated = 1;
    client->auth_deadline_ms = 0;
    client->last_recv_ms = mg_millis();
    char *ok = protocol_json_build_auth_ok();
    send_text(c, ok);
    free(ok);
}

static void handle_authed_message(struct mg_connection *c, const char *json, size_t len) {
    clipsync_msg_type type = protocol_json_type(json, len);
    if (type == CLIPSYNC_MSG_PING) {
        char *pong = protocol_json_build_pong();
        send_text(c, pong);
        free(pong);
        return;
    }
    if (type == CLIPSYNC_MSG_CLIPBOARD_SET) {
        char *text = protocol_json_get_text(json, len);
        if (text && g_on_set) {
            g_on_set(text);
        }
        free(text);
        return;
    }
    (void)c;
}

static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_WAKEUP) {
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (mg_match(hm->uri, mg_str("/"), NULL) || mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else {
            mg_http_reply(c, 404, "", "not found\n");
        }
        return;
    }

    if (ev == MG_EV_WS_OPEN) {
        ws_client *client = client_alloc(c);
        if (!client || make_challenge(client->challenge) != 0) {
            c->is_closing = 1;
            return;
        }
        char *hello = protocol_json_build_hello(client->challenge);
        send_text(c, hello);
        free(hello);
        return;
    }

    if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        ws_client *client = client_find(c);
        if (!client) {
            c->is_closing = 1;
            return;
        }
        client->last_recv_ms = mg_millis();
        if (!client->authenticated) {
            handle_auth(c, client, wm->data.buf, wm->data.len);
        } else {
            handle_authed_message(c, wm->data.buf, wm->data.len);
        }
        return;
    }

    if (ev == MG_EV_CLOSE) {
        client_free(c);
    }
}

int ws_server_init(int port, const char *secret) {
    char listen_url[64];
    if (g_initialized) return 0;

    memset(g_clients, 0, sizeof(g_clients));
    memset(g_secret, 0, sizeof(g_secret));
    if (secret) {
        strncpy(g_secret, secret, sizeof(g_secret) - 1);
    }

    mg_mgr_init(&g_mgr);
    if (mg_wakeup_init(&g_mgr)) {
        g_wakeup_enabled = 1;
    } else {
        g_wakeup_enabled = 0;
        fprintf(stderr, "[ws_server] mg_wakeup_init failed; clipboard wakeups may wait for poll timeout\n");
    }

    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", port);
    g_listener = mg_http_listen(&g_mgr, listen_url, ws_event_handler, NULL);
    if (!g_listener) {
        fprintf(stderr, "[ws_server] failed to listen on %s\n", listen_url);
        mg_mgr_free(&g_mgr);
        return -1;
    }

    g_initialized = 1;
    printf("[ws_server] listening on %s\n", listen_url);
    return 0;
}

void ws_server_poll(int timeout_ms) {
    if (g_initialized) {
        close_expired_auth_clients();
        mg_mgr_poll(&g_mgr, timeout_ms);
    }
}

void ws_server_broadcast(const char *json) {
    if (!g_initialized || !json) return;
    size_t len = strlen(json);
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].conn && g_clients[i].authenticated) {
            mg_ws_send(g_clients[i].conn, json, len, WEBSOCKET_OP_TEXT);
        }
    }
}

void ws_server_set_on_set(ws_on_set_fn fn) {
    g_on_set = fn;
}

void ws_server_wakeup(void) {
    if (g_initialized && g_wakeup_enabled && g_listener) {
        (void)mg_wakeup(&g_mgr, g_listener->id, "clip", 4);
    }
}

int ws_server_next_timeout_ms(int max_idle_ms) {
    unsigned long long now;
    int timeout = max_idle_ms < 0 ? 0 : max_idle_ms;

    if (!g_initialized) return timeout;

    now = mg_millis();
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].conn) continue;

        if (!g_clients[i].authenticated) {
            if (g_clients[i].auth_deadline_ms == 0) continue;
            if (now >= g_clients[i].auth_deadline_ms) return 0;
            unsigned long long remaining = g_clients[i].auth_deadline_ms - now;
            if (remaining < (unsigned long long)timeout) {
                timeout = (int)remaining;
            }
        } else {
            if (g_clients[i].last_recv_ms == 0) continue;
            unsigned long long idle = now - g_clients[i].last_recv_ms;
            if (idle >= AUTHED_IDLE_TIMEOUT_MS) return 0;
            unsigned long long remaining = AUTHED_IDLE_TIMEOUT_MS - idle;
            if (remaining < (unsigned long long)timeout) {
                timeout = (int)remaining;
            }
        }
    }
    return timeout;
}

struct mg_mgr *ws_server_mgr(void) {
    return g_initialized ? &g_mgr : NULL;
}

size_t ws_server_authenticated_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].conn && g_clients[i].authenticated) {
            count++;
        }
    }
    return count;
}
