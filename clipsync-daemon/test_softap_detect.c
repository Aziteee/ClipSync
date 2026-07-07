#include "softap_detect.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    failures++;
}

/* Non-zero sentinel values standing in for IPv4 addresses. Exact values
 * don't matter for the selection logic, only that they're non-zero. */
#define IPV4_STA   0x01020304u
#define IPV4_SAP   0x05060708u

static void test_candidate_name_matches_aosp_regex(void) {
    /* AOSP tetherableWifiRegexs: wlan\d, softap\d, ap_br_wlan\d, ap_br_softap\d */
    if (!softap_iface_is_candidate_name("wlan0")) fail("wlan0 must be candidate");
    if (!softap_iface_is_candidate_name("wlan2")) fail("wlan2 must be candidate");
    if (!softap_iface_is_candidate_name("wlan12")) fail("wlan12 must be candidate");
    if (!softap_iface_is_candidate_name("softap0")) fail("softap0 must be candidate");
    if (!softap_iface_is_candidate_name("ap_br_wlan0")) fail("ap_br_wlan0 must be candidate");
    if (!softap_iface_is_candidate_name("ap_br_softap0")) fail("ap_br_softap0 must be candidate");

    if (softap_iface_is_candidate_name("rmnet_data0")) fail("rmnet must not be candidate");
    if (softap_iface_is_candidate_name("lo")) fail("lo must not be candidate");
    if (softap_iface_is_candidate_name("dummy0")) fail("dummy0 must not be candidate");
    if (softap_iface_is_candidate_name("p2p0")) fail("p2p0 must not be candidate (P2P has its own regex)");
    if (softap_iface_is_candidate_name("wifi-aware0")) fail("wifi-aware0 must not be candidate");
    if (softap_iface_is_candidate_name("wlan")) fail("wlan without digit must not be candidate");
    if (softap_iface_is_candidate_name("wlanabc")) fail("wlanabc must not be candidate");
    if (softap_iface_is_candidate_name("")) fail("empty string must not be candidate");
    if (softap_iface_is_candidate_name("eth0")) fail("eth0 must not be candidate");
    if (softap_iface_is_candidate_name("ap_br_wlan")) fail("ap_br_wlan without digit must not be candidate");
}

static void test_select_prefers_local_network_routed(void) {
    /* Scenario: wlan0 (STA, up, default route) and wlan2 (SAP, up, local_network) */
    struct softap_iface ifaces[] = {
        { "wlan0", IPV4_STA, 1, 0 }, /* 192.168.0.1, not in local_network */
        { "wlan2", IPV4_SAP,  1, 1 }, /* 10.152.10.208, in local_network */
    };
    int idx = softap_select(ifaces, 2, "wlan0");
    if (idx != 1) fail("should select wlan2 (in_local_network_table)");
}

static void test_select_returns_negative_when_none(void) {
    struct softap_iface ifaces[] = {
        { "wlan0", 0, 0, 0 }, /* down */
    };
    int idx = softap_select(ifaces, 1, NULL);
    if (idx != -1) fail("should return -1 when no valid candidate");
}

static void test_select_skips_down_interface(void) {
    struct softap_iface ifaces[] = {
        { "wlan2", IPV4_SAP, 0, 1 }, /* down */
        { "softap0", IPV4_STA, 1, 0 }, /* up but not local_network */
    };
    int idx = softap_select(ifaces, 2, NULL);
    if (idx != 1) fail("should skip down wlan2 and fall back to softap0");
}

static void test_select_skips_no_ipv4(void) {
    struct softap_iface ifaces[] = {
        { "wlan2", 0, 1, 1 }, /* up but no IPv4 */
    };
    int idx = softap_select(ifaces, 1, NULL);
    if (idx != -1) fail("should reject interface without IPv4");
}

static void test_select_fallback_excludes_default_route(void) {
    /* No local_network info available (old Android / OEM mod).
       Must pick the up candidate that is NOT the default route. */
    struct softap_iface ifaces[] = {
        { "wlan0", IPV4_STA, 1, 0 }, /* default route (exclude) */
        { "wlan2", IPV4_SAP,  1, 0 }, /* the SAP */
    };
    int idx = softap_select(ifaces, 2, "wlan0");
    if (idx != 1) fail("fallback should exclude default-route iface and pick wlan2");
}

static void test_select_fallback_returns_negative_if_only_default_route(void) {
    struct softap_iface ifaces[] = {
        { "wlan0", IPV4_STA, 1, 0 },
    };
    int idx = softap_select(ifaces, 1, "wlan0");
    if (idx != -1) fail("should return -1 when only candidate is the excluded default route");
}

static void test_select_prefers_local_network_even_when_default_not_excluded(void) {
    /* exclude_name is NULL but local_network info present: still prefer local_network */
    struct softap_iface ifaces[] = {
        { "wlan0", IPV4_STA, 1, 0 },
        { "wlan2", IPV4_SAP,  1, 1 },
    };
    int idx = softap_select(ifaces, 2, NULL);
    if (idx != 1) fail("should prefer local_network candidate even without exclude");
}

static void test_select_handles_empty_array(void) {
    int idx = softap_select(NULL, 0, NULL);
    if (idx != -1) fail("empty array must return -1");
}

static void test_select_first_match_wins_on_tie(void) {
    /* Two candidates both in local_network (unusual but possible with bridged APs):
       pick the first one deterministically */
    struct softap_iface ifaces[] = {
        { "softap0", IPV4_STA, 1, 1 },
        { "wlan2",   IPV4_SAP,  1, 1 },
    };
    int idx = softap_select(ifaces, 2, NULL);
    if (idx != 0) fail("first matching candidate should win on tie");
}

int main(void) {
    test_candidate_name_matches_aosp_regex();
    test_select_prefers_local_network_routed();
    test_select_returns_negative_when_none();
    test_select_skips_down_interface();
    test_select_skips_no_ipv4();
    test_select_fallback_excludes_default_route();
    test_select_fallback_returns_negative_if_only_default_route();
    test_select_prefers_local_network_even_when_default_not_excluded();
    test_select_handles_empty_array();
    test_select_first_match_wins_on_tie();

    if (failures == 0) {
        printf("softap_detect tests passed\n");
        return 0;
    }
    return 1;
}
