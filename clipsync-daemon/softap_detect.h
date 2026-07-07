#ifndef SOFTAP_DETECT_H
#define SOFTAP_DETECT_H

#include <stdint.h>

#define SOFTAP_IFACE_NAME_MAX 16

/* Candidate interface info.
 * Filled by the caller via getifaddrs() + netlink route queries.
 * Kept as plain struct so the selection logic is pure and unit-testable.
 *
 * ipv4 is stored in network byte order (as returned by getifaddrs / struct
 * in_addr.s_addr). A value of 0 means "no IPv4 address". */
struct softap_iface {
    char name[SOFTAP_IFACE_NAME_MAX];  /* e.g. "wlan2", "softap0" */
    uint32_t ipv4;                     /* IPv4 in network byte order, 0 if none */
    int is_up;                         /* 1 if IFF_UP set */
    int in_local_network_table;        /* 1 if a route for this iface exists
                                          in Android's "local_network" table */
};

/* Returns 1 if `name` matches the AOSP tetherableWifiRegexs
 * (wlan\d, softap\d, ap_br_wlan\d, ap_br_softap\d), else 0. */
int softap_iface_is_candidate_name(const char *name);

/* Select the SoftAP interface from an array of candidates.
 *
 * Selection rules (in order):
 *   1. name must be a candidate name (softap_iface_is_candidate_name)
 *   2. is_up must be 1
 *   3. ipv4 must be non-zero
 *   4. prefer a candidate with in_local_network_table == 1
 *   5. fallback: prefer a candidate whose name != exclude_name
 *      (exclude_name is the default-route interface, may be NULL)
 *
 * Returns the index of the selected interface, or -1 if none qualifies.
 * On a tie, the first matching candidate wins. */
int softap_select(const struct softap_iface *ifaces, int count,
                  const char *exclude_name);

/* Live detection: enumerate interfaces via getifaddrs(), query the
 * local_network routing table via netlink, and return the selected
 * SoftAP interface info in `out`. exclude_name is the default-route
 * interface name (may be NULL).
 *
 * Returns 0 on success (out filled), -1 on failure (no SoftAP found). */
int softap_detect(struct softap_iface *out, const char *exclude_name);

/* Ensure multicast traffic for 224.0.0.0/24 egresses via the given
 * interface by adding a route to Android's "local_network" policy table.
 *
 * On Android, the kernel's default multicast route lookup picks the
 * mobile-data interface (rmnet_data*), not the SoftAP interface, because
 * only the rmnet_data*_local tables contain 224.0.0.0/24 entries. Adding
 * the route to local_network (which the Tethering module associates with
 * the SoftAP iface via ip-rule) makes multicast egress follow the SoftAP.
 *
 * Safe to call repeatedly: if the route already exists, returns 0.
 *
 * Returns 0 on success, -1 on failure. */
int softap_setup_multicast_route(const char *iface_name);

#endif
