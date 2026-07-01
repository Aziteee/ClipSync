#ifndef WS_SERVER_H
#define WS_SERVER_H

typedef void (*ws_on_set_fn)(const char *text);

int  ws_server_init(int port, const char *secret);
void ws_server_poll(int timeout_ms);
void ws_server_broadcast(const char *json);
void ws_server_set_on_set(ws_on_set_fn fn);

#endif
