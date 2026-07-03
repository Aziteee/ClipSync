/* Legacy direct Binder diagnostic.
 *
 * This tool probes Android's clipboard Binder service directly through libbinder_ndk.
 * It is not part of the main sync path, which uses clipsyncd -> @clipbridge ->
 * Zygisk/JNI -> IClipboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#define TRANSACTION_GET_PRIMARY_CLIP  4
#define TRANSACTION_SET_PRIMARY_CLIP  1

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

static void write_clip_plain_text(AParcel *dest, const char *label, const char *text) {
    /* ClipDescription */
    AParcel_writeInt32(dest, 0);  /* TextUtils type=plain */
    AParcel_writeString(dest, label, (int32_t)strlen(label));
    AParcel_writeInt32(dest, 1);  /* mimeTypes count */
    AParcel_writeString(dest, "text/plain", 10);
    AParcel_writeInt32(dest, 0);  /* extras: empty PersistableBundle */
    AParcel_writeInt64(dest, 0LL);/* mTimeStamp */
    AParcel_writeInt32(dest, 0);  /* isStyledText */
    AParcel_writeInt32(dest, 2);  /* classificationStatus=NOT_PERFORMED */
    AParcel_writeInt32(dest, -1); /* confidenceBundle=null */
    /* ClipData header */
    AParcel_writeInt32(dest, 0);  /* hasIcon=0 */
    AParcel_writeInt32(dest, 1);  /* N items */
    /* Item[0] */
    AParcel_writeInt32(dest, 0);  /* TextUtils type=plain */
    if (text && text[0])
        AParcel_writeString(dest, text, (int32_t)strlen(text));
    else
        AParcel_writeString(dest, NULL, 0);
    AParcel_writeInt32(dest, -1); /* htmlText=null (String8) */
    AParcel_writeInt32(dest, 0);  /* hasIntent=0 */
    AParcel_writeInt32(dest, 0);  /* hasIntentSender=0 */
    AParcel_writeInt32(dest, 0);  /* hasUri=0 */
    AParcel_writeInt32(dest, 0);  /* hasActivityInfo=0 */
    AParcel_writeInt32(dest, 0);  /* hasTextLinks=0 */
}

static char *read_clip_plain_text(AParcel *parcel) {
    int32_t dummy;
    AParcel_readInt32(parcel, &dummy); if (dummy == 0) { char *s=NULL; AParcel_readString(parcel,&s,alloc_str); if(s)free(s); }
    int32_t mc; AParcel_readInt32(parcel,&mc);
    for(int32_t i=0;i<mc;i++){ char *s=NULL; AParcel_readString(parcel,&s,alloc_str); if(s)free(s); }
    int32_t ec; AParcel_readInt32(parcel,&ec);
    for(int32_t i=0;i<ec;i++){ char *s=NULL; AParcel_readString(parcel,&s,alloc_str); if(s)free(s); AParcel_readInt32(parcel,&dummy); }
    int64_t ts; AParcel_readInt64(parcel,&ts);
    AParcel_readInt32(parcel,&dummy);
    AParcel_readInt32(parcel,&dummy);
    int32_t cb; AParcel_readInt32(parcel,&cb);
    if(cb>0){ for(int32_t i=0;i<cb;i++){ char *s=NULL; AParcel_readString(parcel,&s,alloc_str); if(s)free(s); AParcel_readInt32(parcel,&dummy); }}
    int32_t hi; AParcel_readInt32(parcel,&hi); if(hi)return NULL;
    int32_t ni; AParcel_readInt32(parcel,&ni);
    char *result = NULL;
    for(int32_t i=0;i<ni;i++) {
        int32_t tt; AParcel_readInt32(parcel,&tt);
        if(tt==0){ char *t=NULL; AParcel_readString(parcel,&t,alloc_str); if(t&&!result)result=strdup(t); if(t)free(t); }
        int32_t hl; AParcel_readInt32(parcel,&hl);
        if(hl>0){ char *s=NULL; AParcel_readString(parcel,&s,alloc_str); if(s)free(s); }
        for(int j=0;j<5;j++){ int32_t ho; AParcel_readInt32(parcel,&ho); if(ho)return result; }
    }
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
    AIBinder_associateClass(svc, cls);
    printf("Ready (no token)\n\n");

    /* Test hasPrimaryClip (code 6) — basic permission check */
    {
        AParcel *d = NULL;
        AIBinder_prepareTransaction(svc, &d);
        AParcel_writeString(d, "com.android.shell", 18);
        AParcel_writeString(d, NULL, 0);
        AParcel_writeInt32(d, 0); AParcel_writeInt32(d, 0);
        AParcel *r = NULL;
        int s = AIBinder_transact(svc, 6 /* hasPrimaryClip */, &d, &r, 0);
        printf("hasPrimaryClip status=%d ", s);
        if (s == STATUS_OK && r) {
            int32_t exc = 0;
            AParcel_readInt32(r, &exc);
            printf("exc=%d ", exc);
            if (exc == 0) {
                int32_t hasResult = 0;
                AParcel_readInt32(r, &hasResult);
                printf("hasResult=%d ", hasResult);
                int32_t val = 0;
                AParcel_readInt32(r, &val);
                printf("val=%d\n", val);
            } else { printf("\n"); }
        }
        if (d) AParcel_delete(d); if (r) AParcel_delete(r);
    }

    /* Read */
    {   AParcel *d = NULL;
        AIBinder_prepareTransaction(svc, &d);
        AParcel_writeString(d, "com.android.shell", 18);
        AParcel_writeString(d, NULL, 0);
        AParcel_writeInt32(d, 0); AParcel_writeInt32(d, 0);
        AParcel *r = NULL;
        int s = AIBinder_transact(svc, TRANSACTION_GET_PRIMARY_CLIP, &d, &r, 0);
        printf("Read status: %d\n", s);
        if (s == STATUS_OK && r) {
            int32_t hr; AParcel_readInt32(r, &hr);
            printf("Has result: %d\n", hr);
            if (hr == 1) {
                char *t = read_clip_plain_text(r);
                if (t) { printf("CLIPBOARD: [%s]\n", t); free(t); }
                else printf("Parse failed\n");
            }
        }
        if (d) AParcel_delete(d); if (r) AParcel_delete(r);
    }

    /* Write */
    const char *test = "ClipSync Binder OK! 你好";
    printf("Write: '%s'\n", test);
    {   AParcel *d = NULL;
        AIBinder_prepareTransaction(svc, &d);
        write_clip_plain_text(d, "ClipSync", test);
        AParcel_writeString(d, "com.android.shell", 18);
        AParcel_writeString(d, NULL, 0);
        AParcel_writeInt32(d, 0); AParcel_writeInt32(d, 0);
        AParcel *r = NULL;
        int s = AIBinder_transact(svc, TRANSACTION_SET_PRIMARY_CLIP, &d, &r, FLAG_ONEWAY);
        printf("Write status: %d\n", s);
        if (d) AParcel_delete(d); if (r) AParcel_delete(r);
    }

    /* Read back */
    printf("Read back:\n");
    {   AParcel *d = NULL;
        AIBinder_prepareTransaction(svc, &d);
        AParcel_writeString(d, "com.android.shell", 18);
        AParcel_writeString(d, NULL, 0);
        AParcel_writeInt32(d, 0); AParcel_writeInt32(d, 0);
        AParcel *r = NULL;
        int s = AIBinder_transact(svc, TRANSACTION_GET_PRIMARY_CLIP, &d, &r, 0);
        if (s == STATUS_OK && r) {
            int32_t hr; AParcel_readInt32(r, &hr);
            if (hr == 1) {
                char *t = read_clip_plain_text(r);
                if (t) { printf("Result: [%s]\n", t); free(t); }
                else printf("Parse failed\n");
            } else { printf("HasResult=%d\n", hr); }
        } else { printf("Read failed: %d\n", s); }
        if (d) AParcel_delete(d); if (r) AParcel_delete(r);
    }
    return 0;
}
