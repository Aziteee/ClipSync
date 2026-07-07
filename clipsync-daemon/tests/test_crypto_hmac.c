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
    char long_key[132];
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

    memset(long_key, (char)0xaa, 131);
    long_key[131] = '\0';
    failures += clipsync_hmac_sha256_hex(
        long_key,
        "Test Using Larger Than Block-Size Key - Hash Key First",
        out
    );
    failures += expect_eq(
        "RFC4231 long key hashes first",
        out,
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
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
