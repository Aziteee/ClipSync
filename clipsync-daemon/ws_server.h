#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stddef.h>

struct mg_mgr;

typedef void (*ws_on_set_fn)(const char *text);

int  ws_server_init(int port, const char *secret);
void ws_server_poll(int timeout_ms);
void ws_server_broadcast(const char *json);
void ws_server_set_on_set(ws_on_set_fn fn);
void ws_server_wakeup(void);
int  ws_server_next_timeout_ms(int max_idle_ms);
struct mg_mgr *ws_server_mgr(void);
size_t ws_server_authenticated_count(void);

#endif
