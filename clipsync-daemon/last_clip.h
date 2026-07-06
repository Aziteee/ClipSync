#ifndef LAST_CLIP_H
#define LAST_CLIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *text;
    size_t len;
    uint64_t hash;
} clipsync_last_clip;

void clipsync_last_clip_init(clipsync_last_clip *clip);
void clipsync_last_clip_free(clipsync_last_clip *clip);
uint64_t clipsync_last_clip_hash(const char *text, size_t len);
int clipsync_last_clip_same_hash(const clipsync_last_clip *clip,
                                 const char *text,
                                 size_t len,
                                 uint64_t hash);
int clipsync_last_clip_same(const clipsync_last_clip *clip,
                            const char *text,
                            size_t len);
int clipsync_last_clip_update(clipsync_last_clip *clip,
                              const char *text,
                              size_t len);

#endif
