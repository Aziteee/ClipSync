#include "last_clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    int failures = 0;
    clipsync_last_clip clip;
    clipsync_last_clip_init(&clip);

    if (clipsync_last_clip_same(&clip, "hello", 5)) {
        failures += fail("empty state must not match");
    }

    if (clipsync_last_clip_update(&clip, "hello", 5) != 0) {
        failures += fail("initial update failed");
    }
    if (!clipsync_last_clip_same(&clip, "hello", 5)) {
        failures += fail("same text must match");
    }
    if (clipsync_last_clip_same(&clip, "world", 5)) {
        failures += fail("different same-length text must not match");
    }

    clip.hash = 1234;
    if (clipsync_last_clip_same_hash(&clip, "jello", 5, 1234)) {
        failures += fail("hash collision must still compare bytes");
    }

    {
        const size_t len = 70000;
        char *a = (char *)malloc(len + 1);
        char *b = (char *)malloc(len + 1);
        if (!a || !b) {
            failures += fail("large allocation failed");
            free(a);
            free(b);
        } else {
            memset(a, 'a', len);
            a[len] = '\0';
            memcpy(b, a, len + 1);

            if (clipsync_last_clip_update(&clip, a, len) != 0) {
                failures += fail("large update failed");
            }
            if (!clipsync_last_clip_same(&clip, b, len)) {
                failures += fail("large same text must match");
            }
            b[len - 1] = 'b';
            if (clipsync_last_clip_same(&clip, b, len)) {
                failures += fail("large text must not be truncated for dedupe");
            }
            free(a);
            free(b);
        }
    }

    clipsync_last_clip_free(&clip);
    if (failures == 0) {
        printf("last_clip tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
