#include "softap_detect.h"

#include <stdio.h>
#include <arpa/inet.h>

int main(void) {
    struct softap_iface iface;
    if (softap_detect(&iface, NULL) != 0) {
        printf("softap_detect: NOT FOUND\n");
        return 1;
    }
    struct in_addr a;
    a.s_addr = iface.ipv4;
    printf("softap_detect: name=%s ipv4=%s is_up=%d in_local_network=%d\n",
           iface.name, inet_ntoa(a), iface.is_up, iface.in_local_network_table);

    /* Also dump all candidates for debugging */
    printf("\n--- /proc/net/route ---\n");
    FILE *f = fopen("/proc/net/route", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) printf("  %s", line);
        fclose(f);
    }
    return 0;
}
