#ifndef CLIPSYNC_CRYPTO_HMAC_H
#define CLIPSYNC_CRYPTO_HMAC_H

#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 64

int clipsync_hmac_sha256_hex(const char *key, const char *message, char out_hex[SHA256_HEX_LEN + 1]);
int clipsync_secure_hex_eq(const char *a, const char *b, size_t n);

#endif
