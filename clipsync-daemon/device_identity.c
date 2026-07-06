#include "device_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>

#define INSTANCE_PREFIX "ClipSync-Android"

static void sanitize_label(char *s) {
    char *start = s;
    char *w = s;
    int last_dash = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (isalnum(ch)) {
            *w++ = (char)ch;
            last_dash = 0;
        } else if (!last_dash && w != start) {
            *w++ = '-';
            last_dash = 1;
        }
    }
    while (w > start && w[-1] == '-') w--;
    *w = '\0';
}

static int read_serial(char *out, size_t out_len) {
    static const char *props[] = {
        "ro.serialno",
        "ro.boot.serialno",
        "ril.serialnumber",
        "ro.vendor.boot.serialno",
    };

    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        int n = __system_property_get(props[i], out);
        if (n > 0 && out[0] != '\0') {
            sanitize_label(out);
            if (out[0] != '\0') return 0;
        }
    }

    snprintf(out, out_len, "unknown");
    return 0;
}

int clipsync_device_instance_name(char *out, size_t out_len) {
    char serial[PROP_VALUE_MAX];

    if (!out || out_len == 0) return -1;

    read_serial(serial, sizeof(serial));
    snprintf(out, out_len, "%s-%s", INSTANCE_PREFIX, serial);
    out[out_len - 1] = '\0';
    return 0;
}
