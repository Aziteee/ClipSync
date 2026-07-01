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

/* --- ClipData parcel helpers --- */

static bool string_alloc(void *cookie, int32_t len, char **buf) {
    if (len <= 0) return false;
    char *s = (char *)malloc((size_t)len);
    if (!s) return false;
    *buf = s;
    *(char **)cookie = s;
    return true;
}

/* Write minimal ClipData for plain text: ClipDescription + 1 text Item */
static void write_clip_plain_text(AParcel *dest, const char *label, const char *text) {
    /* ClipDescription */
    AParcel_writeString(dest, label, (int32_t)strlen(label)); /* mLabel */
    AParcel_writeInt32(dest, 1);                              /* mMimeTypes count = 1 */
    AParcel_writeString(dest, "text/plain", 10);              /* mMimeTypes[0] */
    AParcel_writeInt64(dest, 0LL);                            /* mTimeStamp */
    AParcel_writeInt32(dest, 0);                              /* isStyledText */
    /* mIcon = null */
    AParcel_writeInt32(dest, 0);
    /* mItems count = 1 */
    AParcel_writeInt32(dest, 1);
    /* Item[0] */
    if (text && text[0]) {
        AParcel_writeInt32(dest, 1);                          /* hasText=1 */
        AParcel_writeString(dest, text, (int32_t)strlen(text));
    } else {
        AParcel_writeInt32(dest, 0);                          /* hasText=0 */
    }
    AParcel_writeString(dest, NULL, 0);                       /* htmlText (null) */
    AParcel_writeInt32(dest, 0);                              /* hasIntent=0 */
    /* hasIntentSender — API 35+ field, write 0 */
    AParcel_writeInt32(dest, 0);
    AParcel_writeInt32(dest, 0);                              /* hasUri=0 */
    AParcel_writeInt32(dest, 0);                              /* hasActivityInfo=0 */
}

/* Read ClipData from parcel, extract text from first item */
static char *read_clip_plain_text(AParcel *parcel) {
    char *label = NULL;
    AParcel_readString(parcel, &label, string_alloc);         /* mLabel */
    if (label) { free(label); label = NULL; }

    int32_t mimeCount = 0;
    AParcel_readInt32(parcel, &mimeCount);                    /* mMimeTypes count */
    for (int32_t i = 0; i < mimeCount; i++) {
        char *mt = NULL;
        AParcel_readString(parcel, &mt, string_alloc);
        if (mt) free(mt);
    }
    int64_t ts = 0;
    AParcel_readInt64(parcel, &ts);                           /* mTimeStamp */
    int32_t isStyled = 0;
    AParcel_readInt32(parcel, &isStyled);                     /* isStyledText */

    /* mIcon */
    int32_t hasIcon = 0;
    AParcel_readInt32(parcel, &hasIcon);
    if (hasIcon) return NULL; /* bitmap too complex to skip */

    /* mItems */
    int32_t numItems = 0;
    AParcel_readInt32(parcel, &numItems);

    char *result = NULL;
    for (int32_t i = 0; i < numItems; i++) {
        int32_t hasText = 0;
        AParcel_readInt32(parcel, &hasText);
        if (hasText) {
            char *txt = NULL;
            AParcel_readString(parcel, &txt, string_alloc);
            if (txt && !result) result = strdup(txt);
            if (txt) free(txt);
        }
        /* htmlText */
        char *html = NULL;
        AParcel_readString(parcel, &html, string_alloc);
        if (html) free(html);
        /* hasIntent */
        int32_t hasIntent = 0;
        AParcel_readInt32(parcel, &hasIntent);
        if (hasIntent) return result; /* can't skip Intent */
        /* hasIntentSender — API 35+ */
        int32_t hasIS = 0;
        AParcel_readInt32(parcel, &hasIS);
        if (hasIS) return result; /* can't skip */
        /* hasUri */
        int32_t hasUri = 0;
        AParcel_readInt32(parcel, &hasUri);
        if (hasUri) return result; /* can't skip Uri */
        /* hasActivityInfo */
        int32_t hasAI = 0;
        AParcel_readInt32(parcel, &hasAI);
        if (hasAI) return result;
    }
    return result;
}

char *binder_clip_get_text(void) {
    AParcel *data = NULL;
    if (AIBinder_prepareTransaction(g_clipboard_svc, &data) != STATUS_OK)
        return NULL;
    AParcel_writeString(data, "com.android.shell", 18); /* callingPackage */
    AParcel_writeString(data, NULL, 0);                  /* attributionTag */
    AParcel_writeInt32(data, 0);                         /* userId */
    AParcel_writeInt32(data, 0);                         /* deviceId */

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(
        g_clipboard_svc, TRANSACTION_GET_PRIMARY_CLIP, &data, &reply, 0);

    char *result = NULL;
    if (status == STATUS_OK && reply) {
        int32_t hasResult = 0;
        AParcel_readInt32(reply, &hasResult);
        if (hasResult == 1) {
            result = read_clip_plain_text(reply);
        }
    }

    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return result;
}

int binder_clip_set_text(const char *text) {
    AParcel *data = NULL;
    if (AIBinder_prepareTransaction(g_clipboard_svc, &data) != STATUS_OK)
        return -1;

    write_clip_plain_text(data, "ClipSync", text);     /* ClipData FIRST */
    AParcel_writeString(data, "com.android.shell", 18); /* callingPackage */
    AParcel_writeString(data, NULL, 0);                 /* attributionTag */
    AParcel_writeInt32(data, 0);                        /* userId */
    AParcel_writeInt32(data, 0);                        /* deviceId */

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(
        g_clipboard_svc, TRANSACTION_SET_PRIMARY_CLIP, &data, &reply, FLAG_ONEWAY);

    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return (status == STATUS_OK) ? 0 : -1;
}

void binder_clip_set_callback(clip_change_cb cb) {
    g_callback = cb;
}
