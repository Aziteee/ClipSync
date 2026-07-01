#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#define TRANSACTION_GET_PRIMARY_CLIP  1
#define TRANSACTION_SET_PRIMARY_CLIP  3

static AIBinder* (*p_AServiceManager_getService)(const char*) = NULL;

static bool string_alloc(void *cookie, int32_t len, char **buf) {
    if (len <= 0) return false;
    char *s = (char *)malloc((size_t)len);
    if (!s) return false;
    *buf = s;
    *(char **)cookie = s;
    return true;
}

static char *get_clipboard_text(AIBinder *svc) {
    AParcel *data = AParcel_create();
    AParcel_writeString(data, "clipsync", 8);
    AParcel_writeInt32(data, 0);
    AParcel_writeInt32(data, 0);

    AParcel *reply = AParcel_create();
    binder_status_t status = AIBinder_transact(svc, TRANSACTION_GET_PRIMARY_CLIP, &data, &reply, 0);
    printf("getPrimaryClip status: %d (0x%x)\n", status, status);

    char *result = NULL;
    if (status == STATUS_OK) {
        int32_t has_clip = -1;
        binder_status_t rs = AParcel_readInt32(reply, &has_clip);
        printf("  has_clip=%d, readInt32 status=%d\n", has_clip, rs);
        if (has_clip) {
            char *text = NULL;
            binder_status_t rs2 = AParcel_readString(reply, &text, string_alloc);
            printf("  readString status=%d, text=%p\n", rs2, (void*)text);
            if (text) {
                result = strdup(text);
                free(text);
            }
        }
    }

    AParcel_delete(data);
    AParcel_delete(reply);
    return result;
}

int main(void) {
    void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen fail: %s\n", dlerror()); return 1; }

    p_AServiceManager_getService = dlsym(h, "AServiceManager_getService");
    if (!p_AServiceManager_getService) { fprintf(stderr, "dlsym fail\n"); return 1; }

    AIBinder *svc = p_AServiceManager_getService("clipboard");
    if (!svc) { fprintf(stderr, "getService clipboard fail\n"); return 1; }
    printf("Service connected OK\n");

    char *text = get_clipboard_text(svc);
    if (text) {
        printf("Clipboard text (%zu chars): %s\n", strlen(text), text);
        free(text);
    } else {
        printf("Clipboard is empty or could not read\n");
    }

    /* Test write */
    printf("Writing test text to clipboard...\n");
    AParcel *wdata = AParcel_create();
    AParcel_writeString(wdata, "clipsync", 8);
    AParcel_writeInt32(wdata, 0);
    AParcel_writeInt32(wdata, 0);
    AParcel_writeString(wdata, "Hello from ClipSync test!", 24);
    AParcel_writeString(wdata, "", 0);
    binder_status_t ws = AIBinder_transact(svc, TRANSACTION_SET_PRIMARY_CLIP, &wdata, NULL, FLAG_ONEWAY);
    AParcel_delete(wdata);
    printf("Write result: %d\n", ws);

    /* Read back */
    char *text2 = get_clipboard_text(svc);
    if (text2) {
        printf("Read back: %s\n", text2);
        free(text2);
    }

    return 0;
}
