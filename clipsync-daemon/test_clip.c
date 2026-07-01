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

static void* proxy_onCreate(void *args) { (void)args; return (void*)0x1; }
static void proxy_onDestroy(void *ud) { (void)ud; }
static binder_status_t proxy_onTransact(AIBinder *b, transaction_code_t c,
    const AParcel *i, AParcel *o) { (void)b;(void)c;(void)i;(void)o; return STATUS_OK; }

static bool alloc_str(void *cookie, int32_t len, char **buf) {
    if (len <= 0) return false;
    char *s = (char *)malloc((size_t)len);
    if (!s) return false;
    *buf = s;
    *(char **)cookie = s;
    return true;
}

/* Write a minimal ClipData parcel for plain text */
static void write_clip_data_plain_text(AParcel *dest, const char *label, const char *text) {
    /* ClipDescription */
    AParcel_writeString(dest, label, (int32_t)strlen(label));           /* mLabel */
    /* mMimeTypes: String array with one element "text/plain" */
    AParcel_writeInt32(dest, 1);                                         /* array length */
    AParcel_writeString(dest, "text/plain", 10);                        /* mime type 0 */
    AParcel_writeInt64(dest, 0LL);                                       /* mTimeStamp */
    AParcel_writeInt32(dest, 0);                                         /* isStyledText=0 */

    /* mIcon = null */
    AParcel_writeInt32(dest, 0);

    /* mItems: 1 item */
    AParcel_writeInt32(dest, 1);                                         /* N items */

    /* Item[0] */
    if (text && text[0]) {
        AParcel_writeInt32(dest, 1);                                     /* hasText=1 */
        AParcel_writeString(dest, text, (int32_t)strlen(text));
    } else {
        AParcel_writeInt32(dest, 0);                                     /* hasText=0 */
    }
    AParcel_writeString(dest, "", 0);                                    /* htmlText (null) */
    AParcel_writeInt32(dest, 0);                                         /* hasIntent=0 */
    AParcel_writeInt32(dest, 0);                                         /* hasUri=0 */
    AParcel_writeInt32(dest, 0);                                         /* hasActivityInfo=0 (for newer API) */
}

/* Read a ClipData and extract the text from the first item */
static char *read_clip_data_text(AParcel *parcel) {
    /* ClipDescription */
    char *label = NULL;
    AParcel_readString(parcel, &label, alloc_str);                     /* mLabel */
    if (label) { free(label); label = NULL; }

    int32_t mimeCount = 0;
    AParcel_readInt32(parcel, &mimeCount);                             /* mimeTypes array length */
    for (int32_t i = 0; i < mimeCount; i++) {
        char *mt = NULL;
        AParcel_readString(parcel, &mt, alloc_str);
        if (mt) free(mt);
    }
    int64_t ts = 0;
    AParcel_readInt64(parcel, &ts);                                     /* mTimeStamp */
    int32_t isStyled = 0;
    AParcel_readInt32(parcel, &isStyled);                               /* isStyledText */

    /* mIcon */
    int32_t hasIcon = 0;
    AParcel_readInt32(parcel, &hasIcon);
    if (hasIcon) {
        /* Skip Bitmap — too complex, shouldn't happen for plain text */
        printf("WARNING: icon present, skipping may corrupt data\n");
    }

    /* mItems */
    int32_t numItems = 0;
    AParcel_readInt32(parcel, &numItems);

    char *result = NULL;
    for (int32_t i = 0; i < numItems; i++) {
        int32_t hasText = 0;
        AParcel_readInt32(parcel, &hasText);
        if (hasText) {
            char *txt = NULL;
            AParcel_readString(parcel, &txt, alloc_str);
            if (txt && !result) {
                result = strdup(txt);
            }
            if (txt) free(txt);
        }
        /* htmlText */
        char *html = NULL;
        AParcel_readString(parcel, &html, alloc_str);
        if (html) free(html);

        int32_t hasIntent = 0;
        AParcel_readInt32(parcel, &hasIntent);
        if (hasIntent) {
            /* Skip Intent — complex */
        }
        int32_t hasUri = 0;
        AParcel_readInt32(parcel, &hasUri);
        if (hasUri) {
            /* Skip Uri */
        }
        /* mActivityInfo (API 35+) */
        int32_t hasAi = 0;
        AParcel_readInt32(parcel, &hasAi);
        if (hasAi) {
            /* Skip ActivityInfo */
        }
    }
    return result;
}

static char *get_clipboard_text(AIBinder *svc) {
    AParcel *data = NULL;
    if (AIBinder_prepareTransaction(svc, &data) != STATUS_OK) return NULL;
    AParcel_writeString(data, "com.android.shell", 18);
    AParcel_writeInt32(data, 0);
    AParcel_writeInt32(data, 0);

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(svc, TRANSACTION_GET_PRIMARY_CLIP, &data, &reply, 0);
    printf("getPrimaryClip status: %d\n", status);

    char *result = NULL;
    if (status == STATUS_OK && reply) {
        /* Try without exception code — some AIDL versions put return value directly */
        result = read_clip_data_text(reply);
        if (result) {
            printf("  read OK (direct)\n");
        }
    }

    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return result;
}

int main(void) {
    void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen fail\n"); return 1; }

    p_AServiceManager_getService = dlsym(h, "AServiceManager_getService");
    if (!p_AServiceManager_getService) { fprintf(stderr, "dlsym fail\n"); return 1; }

    AIBinder *svc = p_AServiceManager_getService("clipboard");
    if (!svc) { fprintf(stderr, "getService fail\n"); return 1; }

    AIBinder_Class *cls = AIBinder_Class_define("android.content.IClipboard",
        proxy_onCreate, proxy_onDestroy, proxy_onTransact);
    if (!cls) { fprintf(stderr, "Class_define fail\n"); return 1; }
    AIBinder_Class_disableInterfaceTokenHeader(cls);
    if (!AIBinder_associateClass(svc, cls)) { fprintf(stderr, "associateClass fail\n"); return 1; }
    printf("Ready (no header)\n\n");

    /* Write: verify by long-pressing in any text field on phone */
    const char *test_text = "Hello from ClipSync Binder! 你好世界";
    printf("Writing: '%s'\n", test_text);

    AParcel *wdata = NULL;
    AIBinder_prepareTransaction(svc, &wdata);
    write_clip_data_plain_text(wdata, "ClipSync", test_text);  /* ClipData FIRST */
    AParcel_writeString(wdata, "com.android.shell", 18);        /* callingPackage */
    AParcel_writeString(wdata, NULL, 0);                        /* attributionTag (null) */
    AParcel_writeInt32(wdata, 0);                               /* userId */
    AParcel_writeInt32(wdata, 0);                               /* deviceId */

    AParcel *wr = NULL;
    binder_status_t ws = AIBinder_transact(svc, TRANSACTION_SET_PRIMARY_CLIP, &wdata, &wr, 0 /* no ONEWAY */);
    printf("Write status: %d %s\n", ws, ws == STATUS_OK ? "OK" : "FAIL");
    if (wdata) AParcel_delete(wdata);
    if (wr) AParcel_delete(wr);

    /* Read back */
    AParcel *rdata = NULL;
    AIBinder_prepareTransaction(svc, &rdata);
    AParcel_writeString(rdata, "com.android.shell", 18);
    AParcel_writeString(rdata, "", 0);    /* attributionTag — REQUIRED on Android 12+ */
    AParcel_writeInt32(rdata, 0);
    AParcel_writeInt32(rdata, 0);

    AParcel *rreply = NULL;
    ws = AIBinder_transact(svc, TRANSACTION_GET_PRIMARY_CLIP, &rdata, &rreply, 0);
    printf("Read status: %d %s\n", ws, ws == STATUS_OK ? "OK" : "FAIL");

    if (ws == STATUS_OK && rreply) {
        /* AIDL getPrimaryClip: hasResult(int) + ClipData if present */
        int32_t hasResult = 0;
        AParcel_readInt32(rreply, &hasResult);
        printf("Has result: %d\n", hasResult);

        if (hasResult == 1) {
            char *txt = read_clip_data_text(rreply);
            if (txt) { printf("Text: [%s]\n", txt); free(txt); }
            else { printf("Failed to parse ClipData\n"); }
        } else {
            printf("Clipboard empty or error (code=%d)\n", hasResult);
        }
    }

    if (rdata) AParcel_delete(rdata);
    if (rreply) AParcel_delete(rreply);

    printf("\nNow try pasting on phone...\n");
    return 0;
}
