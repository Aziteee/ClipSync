#ifndef MDNS_PUBLISH_H
#define MDNS_PUBLISH_H

struct mg_mgr;

int mdns_publish_init(struct mg_mgr *mgr, int port, const char *instance_name);
int mdns_publish_announce(void);

#endif
