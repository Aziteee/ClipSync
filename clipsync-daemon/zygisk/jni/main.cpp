/* ClipSync Zygisk Module — clipboard_bridge Binder service
 *
 * Architecture:
 *   Zygisk injects into system_server
 *   → Registers a new Binder service "clipboard_bridge"
 *   → Bridge calls local ClipboardService via JNI (no Binder IPC, no permission check)
 *   → clipsyncd (native daemon) calls clipboard_bridge instead of clipboard
 */

#include "zygisk.hpp"
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "ClipSyncBridge"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Transaction codes — simple interface:
 *   1: getClipText → returns String (or empty)
 *   2: setClipText  → takes String callingPackage + String text
 *   3: hasClip      → returns int32 (1/0)
 */
#define BRIDGE_GET_TEXT  1
#define BRIDGE_SET_TEXT  2
#define BRIDGE_HAS_CLIP  3

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static JNIEnv *g_env = nullptr;

/* --- JNI: Call the local ClipboardService directly (same process, no Binder IPC) --- */

static jobject getClipboardService() {
    if (!g_env) return nullptr;

    // ServiceManager.getService("clipboard") returns the LOCAL binder in system_server
    jclass sm = g_env->FindClass("android/os/ServiceManager");
    if (!sm) { LOGE("ServiceManager class not found"); return nullptr; }

    jmethodID getService = g_env->GetStaticMethodID(
        sm, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (!getService) { LOGE("getService method not found"); return nullptr; }

    jstring name = g_env->NewStringUTF("clipboard");
    jobject binder = g_env->CallStaticObjectMethod(sm, getService, name);
    g_env->DeleteLocalRef(name);
    if (!binder) { LOGE("clipboard service not found"); return nullptr; }

    // Cast to IClipboard.Stub.asInterface (gets the LOCAL stub, not proxy)
    jclass stub = g_env->FindClass("android/content/IClipboard$Stub");
    if (!stub) { LOGE("IClipboard$Stub not found"); return nullptr; }

    jmethodID asInterface = g_env->GetStaticMethodID(
        stub, "asInterface", "(Landroid/os/IBinder;)Landroid/content/IClipboard;");
    if (!asInterface) { LOGE("asInterface not found"); return nullptr; }

    jobject clipboard = g_env->CallStaticObjectMethod(stub, asInterface, binder);
    g_env->DeleteLocalRef(binder);
    g_env->DeleteLocalRef(stub);
    g_env->DeleteLocalRef(sm);

    return clipboard;
}

static char *readClipboardText() {
    jobject svc = getClipboardService();
    if (!svc) return nullptr;

    jclass iclip = g_env->GetObjectClass(svc);
    // getPrimaryClip(String pkg, String tag, int userId, int deviceId)
    jmethodID getPrimary = g_env->GetMethodID(
        iclip, "getPrimaryClip",
        "(Ljava/lang/String;Ljava/lang/String;II)Landroid/content/ClipData;");
    if (!getPrimary) {
        // Try old signature: getPrimaryClip(String pkg, int userId)
        getPrimary = g_env->GetMethodID(
            iclip, "getPrimaryClip",
            "(Ljava/lang/String;I)Landroid/content/ClipData;");
    }
    if (!getPrimary) {
        LOGE("getPrimaryClip method not found");
        g_env->DeleteLocalRef(iclip);
        g_env->DeleteLocalRef(svc);
        return nullptr;
    }

    jstring pkg = g_env->NewStringUTF("com.android.shell");
    jobject clipData = nullptr;

    // Try with 4 args first (newer API), fall back to 2 args
    jclass cdClass = g_env->FindClass("android/content/ClipData");
    if (!cdClass) { LOGE("ClipData class not found"); g_env->DeleteLocalRef(pkg); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return nullptr; }

    // Determine signature: count params in the method name
    // If method name contains "Ljava/lang/String;Ljava/lang/String;II", it's 4 args
    // Otherwise it's the old 2-arg version
    clipData = g_env->CallObjectMethod(svc, getPrimary, pkg, nullptr, (jint)0, (jint)0);

    char *result = nullptr;
    if (clipData) {
        // ClipData.getItemAt(0).getText().toString()
        jmethodID getItemCount = g_env->GetMethodID(cdClass, "getItemCount", "()I");
        jint count = g_env->CallIntMethod(clipData, getItemCount);
        if (count > 0) {
            jmethodID getItemAt = g_env->GetMethodID(cdClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
            jobject item = g_env->CallObjectMethod(clipData, getItemAt, (jint)0);
            if (item) {
                jclass itemClass = g_env->GetObjectClass(item);
                jmethodID getText = g_env->GetMethodID(itemClass, "getText", "()Ljava/lang/CharSequence;");
                jobject cs = g_env->CallObjectMethod(item, getText);
                if (cs) {
                    jmethodID toString = g_env->GetMethodID(
                        g_env->GetObjectClass(cs), "toString", "()Ljava/lang/String;");
                    jstring js = (jstring)g_env->CallObjectMethod(cs, toString);
                    if (js) {
                        const char *utf = g_env->GetStringUTFChars(js, nullptr);
                        if (utf) result = strdup(utf);
                        g_env->ReleaseStringUTFChars(js, utf);
                        g_env->DeleteLocalRef(js);
                    }
                    g_env->DeleteLocalRef(cs);
                }
                g_env->DeleteLocalRef(item);
                g_env->DeleteLocalRef(itemClass);
            }
        }
        g_env->DeleteLocalRef(clipData);
    }
    g_env->DeleteLocalRef(pkg);
    g_env->DeleteLocalRef(iclip);
    g_env->DeleteLocalRef(svc);
    g_env->DeleteLocalRef(cdClass);

    return result;
}

static int writeClipboardText(const char *text) {
    if (!text) return -1;
    jobject svc = getClipboardService();
    if (!svc) return -1;

    jclass iclip = g_env->GetObjectClass(svc);

    // setPrimaryClip(ClipData clip, String pkg, String tag, int userId, int deviceId)
    jmethodID setPrimary = g_env->GetMethodID(
        iclip, "setPrimaryClip",
        "(Landroid/content/ClipData;Ljava/lang/String;Ljava/lang/String;II)V");
    if (!setPrimary) {
        // Old: setPrimaryClip(ClipData clip, String pkg, int userId)
        setPrimary = g_env->GetMethodID(
            iclip, "setPrimaryClip",
            "(Landroid/content/ClipData;Ljava/lang/String;I)V");
    }
    if (!setPrimary) {
        LOGE("setPrimaryClip method not found");
        g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
        return -1;
    }

    // Build ClipData.newPlainText("ClipSync", text)
    jclass cd = g_env->FindClass("android/content/ClipData");
    jmethodID newPlainText = g_env->GetStaticMethodID(
        cd, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    if (!newPlainText) {
        LOGE("newPlainText not found");
        g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
        return -1;
    }

    jstring label = g_env->NewStringUTF("ClipSync");
    jstring jtext = g_env->NewStringUTF(text);
    jobject clipData = g_env->CallStaticObjectMethod(cd, newPlainText, label, jtext);
    g_env->DeleteLocalRef(label);
    g_env->DeleteLocalRef(jtext);

    if (!clipData) {
        g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
        return -1;
    }

    jstring pkg = g_env->NewStringUTF("com.android.shell");
    g_env->CallVoidMethod(svc, setPrimary, clipData, pkg, nullptr, (jint)0, (jint)0);

    g_env->DeleteLocalRef(pkg);
    g_env->DeleteLocalRef(clipData);
    g_env->DeleteLocalRef(cd);
    g_env->DeleteLocalRef(iclip);
    g_env->DeleteLocalRef(svc);

    return 0;
}

/* --- Binder bridge service implementation --- */

static void* bridge_onCreate(void *args) { (void)args; return (void*)0x1; }
static void bridge_onDestroy(void *ud) { (void)ud; }

static bool parcel_alloc(void *cookie, int32_t len, char **buf) {
    if (len <= 0) return false;
    char *s = (char *)malloc((size_t)len);
    if (!s) return false;
    *buf = s;
    *(char **)cookie = s;
    return true;
}

static binder_status_t bridge_onTransact(
    AIBinder *binder,
    transaction_code_t code,
    const AParcel *in,
    AParcel *out)
{
    (void)binder;

    switch (code) {
    case BRIDGE_GET_TEXT: {
        char *text = readClipboardText();
        if (text) {
            AParcel_writeInt32(out, 1);  // has text
            AParcel_writeString(out, text, (int32_t)strlen(text));
            free(text);
        } else {
            AParcel_writeInt32(out, 0);  // no text
        }
        return STATUS_OK;
    }
    case BRIDGE_SET_TEXT: {
        // Read callingPackage (skip), then text
        char *pkg = nullptr;
        AParcel_readString(in, &pkg, parcel_alloc);
        if (pkg) free(pkg);

        char *text = nullptr;
        AParcel_readString(in, &text, parcel_alloc);
        if (!text) return STATUS_BAD_VALUE;

        int ret = writeClipboardText(text);
        free(text);
        AParcel_writeInt32(out, ret == 0 ? 1 : 0);
        return STATUS_OK;
    }
    case BRIDGE_HAS_CLIP: {
        char *text = readClipboardText();
        AParcel_writeInt32(out, text ? 1 : 0);
        if (text) free(text);
        return STATUS_OK;
    }
    default:
        return STATUS_UNKNOWN_TRANSACTION;
    }
}

/* Dynamically resolve AServiceManager_addService (not in NDK stubs) */
static void registerBridgeService() {
    void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) { LOGE("dlopen libbinder_ndk failed"); return; }

    typedef AIBinder* (*addServiceFn)(const char*, AIBinder*);
    auto addService = (addServiceFn)dlsym(h, "AServiceManager_addService");
    if (!addService) {
        // Try alternative names
        addService = (addServiceFn)dlsym(h, "_Z28AServiceManager_addServicePKcP7AIBinder");
    }
    if (!addService) { LOGE("addService symbol not found"); return; }

    AIBinder_Class *cls = AIBinder_Class_define(
        "clipboard_bridge",
        bridge_onCreate,
        bridge_onDestroy,
        bridge_onTransact);
    if (!cls) { LOGE("Class_define failed"); return; }

    AIBinder *binder = AIBinder_new(cls, nullptr);
    if (!binder) { LOGE("AIBinder_new failed"); return; }

    AIBinder *result = addService("clipboard_bridge", binder);
    if (result) {
        LOGD("clipboard_bridge service registered successfully");
    } else {
        LOGE("addService failed");
    }
}

/* --- Zygisk Module Entry --- */

class ClipSyncModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        (void)api;
        g_env = env;
        LOGD("ClipSync Zygisk module loaded");
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        (void)args;
        LOGD("system_server specialize — registering clipboard_bridge");
        registerBridgeService();
    }
};

REGISTER_ZYGISK_MODULE(ClipSyncModule)
