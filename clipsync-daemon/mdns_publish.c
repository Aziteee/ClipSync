#include "mdns_publish.h"
#include "protocol.h"
#include "mongoose.h"
#include <stdio.h>
#include <string.h>
#include "logger.h"

#define CLIPSYNC_MDNS_SERVICE "_clipsync._tcp"
#define CLIPSYNC_MDNS_SERVICE_LOCAL "_clipsync._tcp.local"

static char g_instance[64] = "ClipSync-Android";
static char g_txt_raw[128];
static size_t g_txt_len = 0;
static struct mg_dnssd_record g_record;
static struct mg_connection *g_mdns = NULL;
static int g_port = 0;
static int txt_add(const char *entry) {
    size_t len = entry ? strlen(entry) : 0;
    if (len == 0 || len > 255 || g_txt_len + 1 + len > sizeof(g_txt_raw)) return -1;
    g_txt_raw[g_txt_len++] = (char)len;
    memcpy(g_txt_raw + g_txt_len, entry, len);
    g_txt_len += len;
    return 0;
}

static int txt_build_default(void) {
    g_txt_len = 0;
    return txt_add("version=1") ||
           txt_add("proto=json") ||
           txt_add("auth=hmac-sha256");
}

static void mdns_event_handler(struct mg_connection *c, int ev, void *ev_data) {
    (void)c;
    if (ev != MG_EV_MDNS_REQ) return;

    struct mg_mdns_req *req = (struct mg_mdns_req *) ev_data;
    if (req->is_listing ||
        mg_strcmp(req->reqname, mg_str(CLIPSYNC_MDNS_SERVICE_LOCAL)) == 0 ||
        mg_strcmp(req->reqname, mg_str(MDNS_SERVICE_TYPE)) == 0) {
        req->r = &g_record;
        req->respname = mg_str(g_instance);
        req->is_resp = true;
    }
}

int mdns_publish_init(struct mg_mgr *mgr, int port, const char *instance_name) {
    if (!mgr) return -1;
    if (instance_name && instance_name[0] != '\0') {
        strncpy(g_instance, instance_name, sizeof(g_instance) - 1);
        g_instance[sizeof(g_instance) - 1] = '\0';
    }

    if (txt_build_default() != 0) {
        log_printf("[mdns] failed to build TXT record\n");
        return -1;
    }

    g_record.srvcproto = mg_str(CLIPSYNC_MDNS_SERVICE);
    g_record.txt = mg_str_n(g_txt_raw, g_txt_len);
    g_record.port = (uint16_t) port;
    g_port = port;

    g_mdns = mg_mdns_listen(mgr, mdns_event_handler, g_instance);
    if (!g_mdns) {
        log_printf("[mdns] failed to start mDNS listener\n");
        return -1;
    }

    log_printf("[mdns] publishing %s as '%s' on port %d\n", CLIPSYNC_MDNS_SERVICE_LOCAL, g_instance, port);
    return 0;
}

int mdns_publish_announce(void) {
    if (!g_mdns || g_port <= 0) return -1;
    return mg_mdns_announce(g_mdns, &g_record, mg_str(g_instance)) ? 0 : -1;
}
