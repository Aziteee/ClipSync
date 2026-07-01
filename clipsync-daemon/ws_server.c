#include "ws_server.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

#define MAX_CLIENTS 4

static int g_port = 5287;
static const char *g_secret = NULL;
static ws_on_set_fn g_on_set = NULL;

int ws_server_init(int port, const char *secret) {
    g_port = port;
    g_secret = secret;
    printf("[ws_server] initialized on port %d\n", port);
    /* mongoose initialization: mg_mgr_init, mg_http_listen, etc. */
    return 0;
}

void ws_server_poll(int timeout_ms) {
    /* mg_mgr_poll(mgr, timeout_ms) */
    (void)timeout_ms;
}

void ws_server_broadcast(const char *json) {
    /* Iterate connected clients, send json via mg_ws_send */
    (void)json;
    printf("[ws_server] broadcast: %s\n", json);
}

void ws_server_set_on_set(ws_on_set_fn fn) {
    g_on_set = fn;
}
