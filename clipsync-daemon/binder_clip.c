/* ClipSync — clipboard_bridge client (simplified interface)
 *
 * The Zygisk module `clipboard_bridge` runs inside system_server and
 * calls the local ClipboardService directly via JNI, bypassing all
 * Binder permission checks.
 *
 * Bridge transaction codes:
 *   1: getClipText  → read int32(hasText) + optional String
 *   2: setClipText  → write String(pkg) + String(text), read int32(ok)
 *   3: hasClip      → read int32(1/0)
 */

#include "binder_clip.h"
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#define BRIDGE_GET_TEXT  1
#define BRIDGE_SET_TEXT  2
#define BRIDGE_HAS_CLIP  3

static AIBinder* (*p_AServiceManager_getService)(const char*) = NULL;
static AIBinder *g_bridge_svc = NULL;
static clip_change_cb g_callback = NULL;
static void *g_proxy_class = NULL;

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

int binder_clip_init(void) {
    void *handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[binder_clip] dlopen libbinder_ndk.so failed\n");
        return -1;
    }

    p_AServiceManager_getService = (AIBinder*(*)(const char*))dlsym(handle, "AServiceManager_getService");
    if (!p_AServiceManager_getService) {
        fprintf(stderr, "[binder_clip] AServiceManager_getService not found\n");
        return -1;
    }

    /* Get the bridge service */
    g_bridge_svc = p_AServiceManager_getService("clipboard_bridge");
    if (!g_bridge_svc) {
        fprintf(stderr, "[binder_clip] clipboard_bridge service not found (is Zygisk module loaded?)\n");
        return -1;
    }
    printf("[binder_clip] connected to clipboard_bridge\n");

    /* Associate proxy class for prepareTransaction */
    g_proxy_class = (void*)AIBinder_Class_define("clipboard_bridge",
        proxy_onCreate, proxy_onDestroy, proxy_onTransact);
    if (!g_proxy_class) {
        fprintf(stderr, "[binder_clip] proxy class failed\n");
        return -1;
    }
    AIBinder_associateClass(g_bridge_svc, (AIBinder_Class*)g_proxy_class);

    /* Register a simple listener if we need change events — poll hasClip for now */
    printf("[binder_clip] initialized (bridge mode)\n");
    return 0;
}

char *binder_clip_get_text(void) {
    AParcel *data = NULL;
    if (AIBinder_prepareTransaction(g_bridge_svc, &data) != STATUS_OK) return NULL;

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(
        g_bridge_svc, BRIDGE_GET_TEXT, &data, &reply, 0);

    char *result = NULL;
    if (status == STATUS_OK && reply) {
        int32_t hasText = 0;
        AParcel_readInt32(reply, &hasText);
        if (hasText) {
            char *txt = NULL;
            AParcel_readString(reply, &txt, alloc_str);
            if (txt) result = strdup(txt);
            if (txt) free(txt);
        }
    }

    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return result;
}

int binder_clip_set_text(const char *text) {
    AParcel *data = NULL;
    if (AIBinder_prepareTransaction(g_bridge_svc, &data) != STATUS_OK) return -1;

    AParcel_writeString(data, "clipbridge", 10); /* callingPackage */
    AParcel_writeString(data, text, (int32_t)strlen(text));

    AParcel *reply = NULL;
    binder_status_t status = AIBinder_transact(
        g_bridge_svc, BRIDGE_SET_TEXT, &data, &reply, FLAG_ONEWAY);

    if (data) AParcel_delete(data);
    if (reply) AParcel_delete(reply);
    return (status == STATUS_OK) ? 0 : -1;
}

void binder_clip_set_callback(clip_change_cb cb) {
    g_callback = cb;
    printf("[binder_clip] callback registered (poll mode)\n");
}
