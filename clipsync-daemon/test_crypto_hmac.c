#include <stdio.h>
#include <string.h>
#include "crypto_hmac.h"

static int expect_eq(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s failed\n got:  %s\n want: %s\n", name, got, want);
        return 1;
    }
    return 0;
}

int main(void) {
    char out[SHA256_HEX_LEN + 1];
    int failures = 0;

    failures += clipsync_hmac_sha256_hex(
        "key",
        "The quick brown fox jumps over the lazy dog",
        out
    );
    failures += expect_eq(
        "RFC4231-like quick brown fox",
        out,
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
    );

    failures += clipsync_hmac_sha256_hex("", "", out);
    failures += expect_eq(
        "empty key empty message",
        out,
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"
    );

    if (!clipsync_secure_hex_eq("abcdef", "abcdef", 6)) {
        fprintf(stderr, "secure compare equality failed\n");
        failures++;
    }
    if (clipsync_secure_hex_eq("abcdef", "abcdeg", 6)) {
        fprintf(stderr, "secure compare inequality failed\n");
        failures++;
    }

    if (failures == 0) {
        printf("crypto_hmac tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
