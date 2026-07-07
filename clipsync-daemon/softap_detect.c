#include "softap_detect.h"

#include <ctype.h>
#include <string.h>

int softap_iface_is_candidate_name(const char *name) {
    if (name == NULL) return 0;
    size_t len = strlen(name);
    if (len == 0) return 0;

    /* Match one of the AOSP tetherableWifiRegexs prefixes, followed by one
     * or more digits, nothing else. */
    static const char *const prefixes[] = {
        "wlan", "softap", "ap_br_wlan", "ap_br_softap"
    };
    const size_t n_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);

    for (size_t i = 0; i < n_prefixes; i++) {
        size_t plen = strlen(prefixes[i]);
        if (len <= plen) continue;
        if (strncmp(name, prefixes[i], plen) != 0) continue;

        /* Require at least one digit after the prefix, all digits. */
        int all_digits = 1;
        for (size_t j = plen; j < len; j++) {
            if (!isdigit((unsigned char) name[j])) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits) return 1;
    }
    return 0;
}

static int qualifies(const struct softap_iface *iface) {
    if (!softap_iface_is_candidate_name(iface->name)) return 0;
    if (!iface->is_up) return 0;
    if (iface->ipv4 == 0) return 0;
    return 1;
}

int softap_select(const struct softap_iface *ifaces, int count,
                  const char *exclude_name) {
    if (ifaces == NULL || count <= 0) return -1;

    /* Pass 1: prefer a qualified candidate in the local_network table. */
    for (int i = 0; i < count; i++) {
        if (!qualifies(&ifaces[i])) continue;
        if (ifaces[i].in_local_network_table) return i;
    }

    /* Pass 2: fallback - any qualified candidate whose name is not the
     * default-route interface. */
    for (int i = 0; i < count; i++) {
        if (!qualifies(&ifaces[i])) continue;
        if (exclude_name == NULL ||
            strncmp(ifaces[i].name, exclude_name, SOFTAP_IFACE_NAME_MAX) != 0) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Live detection (Linux/Android only).
 *
 * Strategy:
 *   1. Enumerate interfaces with getifaddrs(), collect those matching the
 *      AOSP tetherable wifi regex and having an IPv4 address.
 *   2. Query the kernel routing table for RT_TABLE_LOCAL (255) is wrong here;
 *      Android's "local_network" table is a policy-routing table identified
 *      by NAME, not a fixed number. We query it by name via the
 *      RTM_GETROUTE netlink request with RTA_TABLE set to the table id
 *      resolved from the name. To keep this dependency-free and robust
 *      across OEM customisation, we use a simpler heuristic that matches
 *      the empirical Android routing layout:
 *
 *      The SoftAP interface is the one whose IPv4 /24 subnet is reachable
 *      directly (scope link) from the SAME interface AND which is NOT the
 *      default-route interface. We additionally boost confidence by
 *      checking /proc/net/route for an entry whose interface matches and
 *      whose gateway is 0 (directly connected) and flags has RTF_UP but
 *      NOT RTF_GATEWAY.
 *
 *   3. Combine with softap_select() for the final decision.
 *
 * This avoids the complexity and fragility of parsing ip-rule output and
 * resolving the symbolic "local_network" table id, while still picking the
 * correct interface in the hotspot scenario.
 * ------------------------------------------------------------------------- */

#ifndef _WIN32

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

/* Parse /proc/net/route and fill the in_local_network_table flag for each
 * candidate. On Android, the SoftAP downstream iface has a directly-
 * connected route entry (gateway == 0) whose destination is its own /24.
 * The mobile upstream iface (rmnet_data*) has a default route (dest==0,
 * gateway != 0). We treat an iface as "in_local_network_table" if it has
 * a directly-connected non-default route. */
static void mark_local_network_ifaces(struct softap_iface *ifaces, int count) {
    FILE *f = fopen("/proc/net/route", "r");
    if (f == NULL) return;

    char line[256];
    /* Skip header line. */
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char iface_name[SOFTAP_IFACE_NAME_MAX];
        unsigned int dest, gw, flags;
        /* /proc/net/route columns: Iface Destination Gateway Flags RefCnt
         * Use Metric Mask MTU Window IRTT */
        if (sscanf(line, "%15s %x %x %x", iface_name, &dest, &gw, &flags) != 4) {
            continue;
        }
        /* RTF_UP = 0x0001, RTF_GATEWAY = 0x0002. We want directly-connected
         * routes: flag UP set, GATEWAY not set, destination non-zero
         * (non-default). On Android these are the per-iface /24 (or /30)
         * link-scoped routes added by the tethering IpServer. */
        if ((flags & 0x1) == 0) continue;       /* not UP */
        if (flags & 0x2) continue;              /* has gateway = not local */
        if (dest == 0) continue;                /* default route */

        for (int i = 0; i < count; i++) {
            if (strncmp(ifaces[i].name, iface_name, SOFTAP_IFACE_NAME_MAX) == 0) {
                ifaces[i].in_local_network_table = 1;
            }
        }
    }
    fclose(f);
}

static void get_default_route_iface(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';

    FILE *f = fopen("/proc/net/route", "r");
    if (f == NULL) return;

    char line[256];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char iface_name[SOFTAP_IFACE_NAME_MAX];
        unsigned int dest, gw, flags;
        if (sscanf(line, "%15s %x %x %x", iface_name, &dest, &gw, &flags) != 4) {
            continue;
        }
        /* Default route: destination == 0, gateway != 0, RTF_UP | RTF_GATEWAY. */
        if (dest == 0 && gw != 0 && (flags & 0x1) && (flags & 0x2)) {
            strncpy(out, iface_name, out_size - 1);
            out[out_size - 1] = '\0';
            break;
        }
    }
    fclose(f);
}

int softap_detect(struct softap_iface *out, const char *exclude_name) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) return -1;

    /* Collect candidates. Cap at 32 - more than enough wifi ifaces. */
    struct softap_iface candidates[32];
    int n = 0;

    for (struct ifaddrs *p = ifap; p != NULL && n < 32; p = p->ifa_next) {
        if (p->ifa_addr == NULL) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if (!softap_iface_is_candidate_name(p->ifa_name)) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *) p->ifa_addr;
        uint32_t ip = sa->sin_addr.s_addr;
        if (ip == 0) continue;

        /* Dedupe by name: getifaddrs returns one entry per address family;
         * we only care about AF_INET so no dup expected, but guard anyway. */
        int dup = 0;
        for (int i = 0; i < n; i++) {
            if (strncmp(candidates[i].name, p->ifa_name,
                        SOFTAP_IFACE_NAME_MAX) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        strncpy(candidates[n].name, p->ifa_name, SOFTAP_IFACE_NAME_MAX - 1);
        candidates[n].name[SOFTAP_IFACE_NAME_MAX - 1] = '\0';
        candidates[n].ipv4 = ip;
        candidates[n].is_up = (p->ifa_flags & IFF_UP) ? 1 : 0;
        candidates[n].in_local_network_table = 0;
        n++;
    }
    freeifaddrs(ifap);

    if (n == 0) return -1;

    mark_local_network_ifaces(candidates, n);

    char default_iface[SOFTAP_IFACE_NAME_MAX] = {0};
    if (exclude_name != NULL) {
        strncpy(default_iface, exclude_name, SOFTAP_IFACE_NAME_MAX - 1);
        default_iface[SOFTAP_IFACE_NAME_MAX - 1] = '\0';
    } else {
        get_default_route_iface(default_iface, sizeof(default_iface));
    }

    int idx = softap_select(candidates, n,
                            default_iface[0] ? default_iface : NULL);
    if (idx < 0) return -1;
    *out = candidates[idx];
    return 0;
}

#else  /* _WIN32 */
int softap_detect(struct softap_iface *out, const char *exclude_name) {
    (void) out;
    (void) exclude_name;
    return -1;
}

int softap_setup_multicast_route(const char *iface_name) {
    (void) iface_name;
    return -1;
}
#endif

#ifndef _WIN32
/* Forks /system/bin/ip to add a multicast route. Returns 0 if the route
 * exists after the call (either pre-existing or just added), -1 otherwise.
 * Uses fork+execvp rather than system() to avoid shell quoting issues and
 * to get a reliable exit status. */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

int softap_setup_multicast_route(const char *iface_name) {
    if (iface_name == NULL || iface_name[0] == '\0') return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child. Redirect stdio to /dev/null to keep daemon output clean. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }
        const char *argv[] = {
            "ip", "route", "add", "224.0.0.0/24",
            "dev", iface_name, "table", "local_network", NULL
        };
        execvp("ip", (char *const *) argv);
        /* If /system/bin/ip isn't on PATH, try the absolute path. */
        const char *argv2[] = {
            "/system/bin/ip", "route", "add", "224.0.0.0/24",
            "dev", iface_name, "table", "local_network", NULL
        };
        execv("/system/bin/ip", (char *const *) argv2);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    /* `ip route add` returns 2 on "RTNETLINK answers: File exists" (route
     * already present), which we treat as success. 0 = added, 2 = exists. */
    int code = WEXITSTATUS(status);
    if (code == 0 || code == 2) return 0;
    return -1;
}
#endif
