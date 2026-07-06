#include "last_clip.h"

#include <stdlib.h>
#include <string.h>

#define FNV1A64_OFFSET 14695981039346656037ULL
#define FNV1A64_PRIME 1099511628211ULL

void clipsync_last_clip_init(clipsync_last_clip *clip) {
    if (!clip) return;
    clip->text = NULL;
    clip->len = 0;
    clip->hash = 0;
}

void clipsync_last_clip_free(clipsync_last_clip *clip) {
    if (!clip) return;
    free(clip->text);
    clipsync_last_clip_init(clip);
}

uint64_t clipsync_last_clip_hash(const char *text, size_t len) {
    uint64_t hash = FNV1A64_OFFSET;
    const unsigned char *p = (const unsigned char *)text;

    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= FNV1A64_PRIME;
    }
    return hash;
}

int clipsync_last_clip_same_hash(const clipsync_last_clip *clip,
                                 const char *text,
                                 size_t len,
                                 uint64_t hash) {
    if (!clip || !clip->text || !text) return 0;
    if (clip->len != len || clip->hash != hash) return 0;
    return memcmp(clip->text, text, len) == 0;
}

int clipsync_last_clip_same(const clipsync_last_clip *clip,
                            const char *text,
                            size_t len) {
    if (!text) return 0;
    return clipsync_last_clip_same_hash(
        clip,
        text,
        len,
        clipsync_last_clip_hash(text, len));
}

int clipsync_last_clip_update(clipsync_last_clip *clip,
                              const char *text,
                              size_t len) {
    char *copy;

    if (!clip || !text) return -1;
    copy = (char *)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, text, len);
    copy[len] = '\0';

    free(clip->text);
    clip->text = copy;
    clip->len = len;
    clip->hash = clipsync_last_clip_hash(text, len);
    return 0;
}
