#include "binder_clip.h"
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

/* Runtime-resolved: linked from system libbinder_ndk.so */
static AIBinder* (*p_AServiceManager_getService)(const char* instance) = NULL;

/* Transaction codes (from AIDL) */
#define TRANSACTION_GET_PRIMARY_CLIP           1
#define TRANSACTION_SET_PRIMARY_CLIP           3
#define TRANSACTION_ADD_PRIMARY_CLIP_CHANGED_LISTENER 4

/* IOnPrimaryClipChangedListener transaction */
#define TRANSACTION_DISPATCH_CLIP_CHANGED      1

static AIBinder *g_clipboard_svc = NULL;
static AIBinder *g_listener = NULL;
static clip_change_cb g_callback = NULL;

static void *g_clip_listener_class = NULL;
static void *g_clipboard_proxy_class = NULL;

static void* proxy_onCreate(void *args) { (void)args; return (void*)0x1; }
static void proxy_onDestroy(void *userData) { (void)userData; }
static binder_status_t proxy_onTransact(AIBinder *binder, transaction_code_t code,
    const AParcel *in, AParcel *out) {
    (void)binder; (void)code; (void)in; (void)out;
    return STATUS_OK;
}

static void* listener_onCreate(void *args) {
    (void)args;
    return (void*)0x1; /* dummy non-null user data */
}

static void listener_onDestroy(void *userData) {
    (void)userData;
}

static binder_status_t on_transact(
    AIBinder *binder,
    transaction_code_t code,
    const AParcel *in,
    AParcel *out)
{
    (void)binder; (void)in; (void)out;
    if (code == TRANSACTION_DISPATCH_CLIP_CHANGED) {
        if (g_callback) {
            char *text = binder_clip_get_text();
            if (text) {
                g_callback(text);
                free(text);
            }
        }
        return STATUS_OK;
    }
    return STATUS_UNKNOWN_TRANSACTION;
}

int binder_clip_init(void) {
    /* Resolve AServiceManager_getService at runtime (NDK stub doesn't export it) */
    void *handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[binder_clip] failed to dlopen libbinder_ndk.so: %s\n", dlerror());
        return -1;
    }
    printf("[binder_clip] dlopen OK\n");

    p_AServiceManager_getService = (AIBinder*(*)(const char*))dlsym(handle, "AServiceManager_getService");
    if (!p_AServiceManager_getService) {
        fprintf(stderr, "[binder_clip] AServiceManager_getService not found: %s\n", dlerror());
        dlclose(handle);
        return -1;
    }
    printf("[binder_clip] dlsym OK\n");

    /* Get the clipboard service */
    g_clipboard_svc = p_AServiceManager_getService("clipboard");
    if (!g_clipboard_svc) {
        fprintf(stderr, "[binder_clip] failed to get clipboard service\n");
        return -1;
    }
    printf("[binder_clip] getService clipboard OK\n");

    /* Associate proxy class so AIBinder_prepareTransaction works */
    g_clipboard_proxy_class = (void*)AIBinder_Class_define(
        "android.content.IClipboard",
        proxy_onCreate,
        proxy_onDestroy,
        proxy_onTransact
    );
    if (!g_clipboard_proxy_class) {
        fprintf(stderr, "[binder_clip] failed to define clipboard proxy class\n");
        return -1;
    }
    if (!AIBinder_associateClass(g_clipboard_svc, (AIBinder_Class*)g_clipboard_proxy_class)) {
        fprintf(stderr, "[binder_clip] failed to associate clipboard class\n");
        return -1;
    }
    printf("[binder_clip] clipboard proxy class associated\n");

    /* Create listener class */
    printf("[binder_clip] calling AIBinder_Class_define...\n");
    g_clip_listener_class = (void*)AIBinder_Class_define(
        "IOnPrimaryClipChangedListener",
        listener_onCreate,
        listener_onDestroy,
        on_transact
    );
    printf("[binder_clip] AIBinder_Class_define returned %p\n", g_clip_listener_class);
    if (!g_clip_listener_class) {
        fprintf(stderr, "[binder_clip] WARNING: failed to define listener class (will retry later)\n");
        /* Don't fail — read/write still works without the listener */
        printf("[binder_clip] initialized (read/write only, no listener)\n");
        return 0;
    }

    /* Create listener instance */
    g_listener = AIBinder_new((AIBinder_Class*)g_clip_listener_class, NULL);
    if (!g_listener) {
        fprintf(stderr, "[binder_clip] failed to create listener\n");
        return -1;
    }

    /* Register listener: addPrimaryClipChangedListener */
    AParcel *data = NULL;
    binder_status_t status = AIBinder_prepareTransaction(g_clipboard_svc, &data);
    if (status != STATUS_OK) {
        fprintf(stderr, "[binder_clip] prepareTransaction failed: %d\n", status);
        return -1;
    }
    AParcel_writeStrongBinder(data, g_listener);
    AParcel_writeString(data, "clipsync", 8);
    AParcel_writeString(data, "", 0);
    AParcel_writeInt32(data, 0); /* userId */
    AParcel_writeInt32(data, 0); /* deviceId */

    AParcel *reply = NULL;
    status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_ADD_PRIMARY_CLIP_CHANGED_LISTENER,
        &data,
        &reply,
        FLAG_ONEWAY
    );
    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);

    if (status != STATUS_OK) {
        fprintf(stderr, "[binder_clip] register listener failed: %d\n", status);
        return -1;
    }

    printf("[binder_clip] initialized, listener registered\n");
    return 0;
}

static bool string_alloc(void *cookie, int32_t len, char **buf) {
    if (len <= 0) return false;
    char *s = (char *)malloc((size_t)len);
    if (!s) return false;
    *buf = s;
    *(char **)cookie = s;
    return true;
}

char *binder_clip_get_text(void) {
    /* transact getPrimaryClip */
    AParcel *data = AParcel_create();
    AParcel_writeString(data, "clipsync", 8); /* callingPackage */
    AParcel_writeInt32(data, 0);              /* userId */
    AParcel_writeInt32(data, 0);              /* deviceId */

    AParcel *reply = AParcel_create();
    binder_status_t status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_GET_PRIMARY_CLIP,
        &data,
        &reply,
        0
    );

    char *result = NULL;
    if (status == STATUS_OK) {
        AParcel_readInt32(reply, NULL); /* read ClipboardData presence flag */
        char *text = NULL;
        AParcel_readString(reply, &text, string_alloc);
        if (text) {
            result = strdup(text);
            free(text);
        }
    }

    AParcel_delete(data);
    AParcel_delete(reply);
    return result;
}

int binder_clip_set_text(const char *text) {
    /* Construct ClipData via transact setPrimaryClip */
    AParcel *data = AParcel_create();
    AParcel_writeString(data, "clipsync", 8); /* callingPackage */
    AParcel_writeInt32(data, 0);              /* userId */
    AParcel_writeInt32(data, 0);              /* deviceId */
    AParcel_writeString(data, text, (int32_t)strlen(text));
    AParcel_writeString(data, "", 0);         /* attributionTag */

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(
        g_clipboard_svc,
        TRANSACTION_SET_PRIMARY_CLIP,
        &data,
        &reply,
        FLAG_ONEWAY
    );

    AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return (status == STATUS_OK) ? 0 : -1;
}

void binder_clip_set_callback(clip_change_cb cb) {
    g_callback = cb;
}
