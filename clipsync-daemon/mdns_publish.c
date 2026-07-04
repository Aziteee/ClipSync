#include "mdns_publish.h"
#include "protocol.h"
#include "mongoose.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIPSYNC_MDNS_SERVICE "_clipsync._tcp"
#define CLIPSYNC_MDNS_SERVICE_LOCAL "_clipsync._tcp.local"

static char g_instance[64] = "ClipSync Android";
static char g_txt_raw[] = "\x09" "version=1" "\x0a" "proto=json" "\x11" "auth=hmac-sha256";
static struct mg_dnssd_record g_record;
static struct mg_connection *g_mdns = NULL;
static int g_port = 0;

static int put_u16(uint8_t **p, uint8_t *end, uint16_t v) {
    if (*p + 2 > end) return -1;
    v = htons(v);
    memcpy(*p, &v, 2);
    *p += 2;
    return 0;
}

static int put_u32(uint8_t **p, uint8_t *end, uint32_t v) {
    if (*p + 4 > end) return -1;
    v = htonl(v);
    memcpy(*p, &v, 4);
    *p += 4;
    return 0;
}

static int put_bytes(uint8_t **p, uint8_t *end, const void *data, size_t len) {
    if (*p + len > end) return -1;
    memcpy(*p, data, len);
    *p += len;
    return 0;
}

static int put_label(uint8_t **p, uint8_t *end, const char *label) {
    size_t len = label ? strlen(label) : 0;
    if (len == 0 || len > 63 || *p + 1 + len > end) return -1;
    *(*p)++ = (uint8_t)len;
    return put_bytes(p, end, label, len);
}

static int put_service_name(uint8_t **p, uint8_t *end) {
    return put_label(p, end, "_clipsync") ||
           put_label(p, end, "_tcp") ||
           put_label(p, end, "local") ||
           put_bytes(p, end, "", 1);
}

static int put_instance_name(uint8_t **p, uint8_t *end) {
    return put_label(p, end, g_instance) ||
           put_service_name(p, end);
}

static int put_host_name(uint8_t **p, uint8_t *end) {
    return put_label(p, end, "ClipSync-Android") ||
           put_label(p, end, "local") ||
           put_bytes(p, end, "", 1);
}

static int put_record_header(uint8_t **p, uint8_t *end, int (*name_fn)(uint8_t **, uint8_t *),
                             uint16_t type, uint16_t klass, uint32_t ttl, uint16_t rdlen) {
    return name_fn(p, end) ||
           put_u16(p, end, type) ||
           put_u16(p, end, klass) ||
           put_u32(p, end, ttl) ||
           put_u16(p, end, rdlen);
}

static int local_ipv4_for_mdns(int fd, struct in_addr *out) {
    struct sockaddr_in dst = {};
    struct sockaddr_in local = {};
    socklen_t local_len = sizeof(local);

    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    if (inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr) != 1) return -1;
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) return -1;
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) != 0) return -1;
    if (local.sin_addr.s_addr == htonl(INADDR_ANY)) return -1;
    *out = local.sin_addr;
    return 0;
}

static int build_announcement(uint8_t *buf, size_t cap, struct in_addr addr, size_t *out_len) {
    uint8_t *p = buf;
    uint8_t *end = buf + cap;
    uint8_t *rdlen_ptr;
    uint8_t *rdata_start;
    uint16_t rdlen;

    if (put_u16(&p, end, 0) || put_u16(&p, end, 0x8400) ||
        put_u16(&p, end, 0) || put_u16(&p, end, 4) ||
        put_u16(&p, end, 0) || put_u16(&p, end, 0)) {
        return -1;
    }

    rdlen = (uint16_t)(strlen(g_instance) + strlen("._clipsync._tcp.local") + 2);
    if (put_record_header(&p, end, put_service_name, 12, 1, 120, rdlen) ||
        put_instance_name(&p, end)) {
        return -1;
    }

    if (put_instance_name(&p, end) ||
        put_u16(&p, end, 33) || put_u16(&p, end, 0x8001) ||
        put_u32(&p, end, 120)) {
        return -1;
    }
    rdlen_ptr = p;
    if (put_u16(&p, end, 0)) return -1;
    rdata_start = p;
    if (put_u16(&p, end, 0) || put_u16(&p, end, 0) ||
        put_u16(&p, end, (uint16_t)g_port) ||
        put_host_name(&p, end)) {
        return -1;
    }
    rdlen = (uint16_t)(p - rdata_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xff);

    if (put_record_header(&p, end, put_instance_name, 16, 0x8001, 120, (uint16_t)(sizeof(g_txt_raw) - 1)) ||
        put_bytes(&p, end, g_txt_raw, sizeof(g_txt_raw) - 1)) {
        return -1;
    }

    if (put_record_header(&p, end, put_host_name, 1, 0x8001, 120, 4) ||
        put_bytes(&p, end, &addr.s_addr, 4)) {
        return -1;
    }

    *out_len = (size_t)(p - buf);
    return 0;
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

    g_record.srvcproto = mg_str(CLIPSYNC_MDNS_SERVICE);
    g_record.txt = mg_str_n(g_txt_raw, sizeof(g_txt_raw) - 1);
    g_record.port = (uint16_t) port;
    g_port = port;

    g_mdns = mg_mdns_listen(mgr, mdns_event_handler, g_instance);
    if (!g_mdns) {
        fprintf(stderr, "[mdns] failed to start mDNS listener\n");
        return -1;
    }

    printf("[mdns] publishing %s as '%s' on port %d\n", CLIPSYNC_MDNS_SERVICE_LOCAL, g_instance, port);
    return 0;
}

int mdns_publish_announce(void) {
    int fd = -1;
    int yes = 1;
    unsigned char ttl = 255;
    struct sockaddr_in bind_addr = {};
    struct sockaddr_in dst = {};
    struct in_addr local_addr = {};
    uint8_t packet[512];
    size_t packet_len = 0;
    int rc = -1;

    if (!g_mdns || g_port <= 0) return -1;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(5353);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0 && errno != EADDRINUSE) {
        goto out;
    }

    if (local_ipv4_for_mdns(fd, &local_addr) != 0 ||
        build_announcement(packet, sizeof(packet), local_addr, &packet_len) != 0) {
        goto out;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    if (inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr) != 1) goto out;

    rc = (sendto(fd, packet, packet_len, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)packet_len) ? 0 : -1;

out:
    close(fd);
    return rc;
}
