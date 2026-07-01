#include "mdns_publish.h"
#include <stdio.h>

int mdns_publish_init(int port) {
    printf("[mdns] publishing _clipsync._tcp on port %d\n", port);
    /* mongoose mdns: mg_mdns_register with service type and port */
    return 0;
}

void mdns_publish_poll(void) {
    /* Driven by mg_mgr_poll in the main loop */
}
