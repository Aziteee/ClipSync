/* ClipSync Zygisk Module — clipboard_bridge Binder service
 *
 * Architecture:
 *   Zygisk injects into system_server
 *   → Registers a new Binder service "clipboard_bridge"
 *   → Bridge calls local ClipboardService via JNI (no Binder IPC, no permission check)
 *   → clipsyncd (native daemon) calls clipboard_bridge instead of clipboard
 */

#include "zygisk.hpp"
#include <android/log.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <jni.h>

#define TAG "ClipSyncBridge"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ===== Binder NDK types (minimal from headers) ===== */
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;
typedef uint32_t binder_flags_t;
#define STATUS_OK     0
#define STATUS_BAD_VALUE (-22)
#define STATUS_UNKNOWN_TRANSACTION (-2147483645)
#define FLAG_ONEWAY   1

struct AIBinder;
struct AIBinder_Class;
struct AParcel;

/* ===== Runtime-resolved Binder NDK functions ===== */
static void *g_binder_handle = nullptr;

static AIBinder_Class* (*_Class_define)(const char*, void*(*)(void*), void(*)(void*),
    binder_status_t(*)(AIBinder*, transaction_code_t, const AParcel*, AParcel*)) = nullptr;
static AIBinder* (*_AIBinder_new)(const AIBinder_Class*, void*) = nullptr;
static void (*_Class_disableToken)(AIBinder_Class*) = nullptr;
static binder_status_t (*_prepareTx)(AIBinder*, AParcel**) = nullptr;
static binder_status_t (*_transact)(AIBinder*, transaction_code_t, AParcel**, AParcel**, binder_flags_t) = nullptr;
static binder_status_t (*_writeString)(AParcel*, const char*, int32_t) = nullptr;
static binder_status_t (*_writeInt32)(AParcel*, int32_t) = nullptr;
static binder_status_t (*_readInt32)(const AParcel*, int32_t*) = nullptr;
static binder_status_t (*_readString)(const AParcel*, void*, bool(*)(void*,int32_t,char**)) = nullptr;
static void (*_parcelDelete)(AParcel*) = nullptr;
static bool (*_associateClass)(AIBinder*, const AIBinder_Class*) = nullptr;

static bool resolve_binder() {
    if (g_binder_handle) return true;
    g_binder_handle = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!g_binder_handle) { LOGE("dlopen libbinder_ndk failed"); return false; }
    *(void**)&_Class_define  = dlsym(g_binder_handle, "AIBinder_Class_define");
    *(void**)&_AIBinder_new  = dlsym(g_binder_handle, "AIBinder_new");
    *(void**)&_Class_disableToken = dlsym(g_binder_handle, "AIBinder_Class_disableInterfaceTokenHeader");
    *(void**)&_prepareTx     = dlsym(g_binder_handle, "AIBinder_prepareTransaction");
    *(void**)&_transact      = dlsym(g_binder_handle, "AIBinder_transact");
    *(void**)&_writeString   = dlsym(g_binder_handle, "AParcel_writeString");
    *(void**)&_writeInt32    = dlsym(g_binder_handle, "AParcel_writeInt32");
    *(void**)&_readInt32     = dlsym(g_binder_handle, "AParcel_readInt32");
    *(void**)&_readString    = dlsym(g_binder_handle, "AParcel_readString");
    *(void**)&_parcelDelete  = dlsym(g_binder_handle, "AParcel_delete");
    *(void**)&_associateClass= dlsym(g_binder_handle, "AIBinder_associateClass");
    if (!_Class_define || !_AIBinder_new || !_prepareTx || !_transact ||
        !_writeString || !_writeInt32 || !_readInt32 || !_readString || !_parcelDelete) {
        LOGE("missing binder symbols"); return false;
    }
    LOGD("binder symbols resolved");
    return true;
}

/* Bridge transaction codes */
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
            _writeInt32(out, 1);
            _writeString(out, text, (int32_t)strlen(text));
            free(text);
        } else {
            _writeInt32(out, 0);
        }
        return STATUS_OK;
    }
    case BRIDGE_SET_TEXT: {
        char *pkg = nullptr;
        _readString(in, &pkg, parcel_alloc);
        if (pkg) free(pkg);

        char *text = nullptr;
        _readString(in, &text, parcel_alloc);
        if (!text) return STATUS_BAD_VALUE;

        int ret = writeClipboardText(text);
        free(text);
        _writeInt32(out, ret == 0 ? 1 : 0);
        return STATUS_OK;
    }
    case BRIDGE_HAS_CLIP: {
        char *text = readClipboardText();
        _writeInt32(out, text ? 1 : 0);
        if (text) free(text);
        return STATUS_OK;
    }
    default:
        return STATUS_UNKNOWN_TRANSACTION;
    }
}

/* Resolve addService and register clipboard_bridge */
static void registerBridgeService() {
    if (!resolve_binder()) return;

    typedef AIBinder* (*addServiceFn)(const char*, AIBinder*);
    auto addService = (addServiceFn)dlsym(g_binder_handle, "AServiceManager_addService");
    if (!addService) {
        addService = (addServiceFn)dlsym(g_binder_handle, "_Z28AServiceManager_addServicePKcP7AIBinder");
    }
    if (!addService) { LOGE("addService symbol not found"); return; }

    AIBinder_Class *cls = _Class_define(
        "clipboard_bridge", bridge_onCreate, bridge_onDestroy, bridge_onTransact);
    if (!cls) { LOGE("Class_define failed"); return; }

    _Class_disableToken(cls);

    AIBinder *binder = _AIBinder_new(cls, nullptr);
    if (!binder) { LOGE("AIBinder_new failed"); return; }

    AIBinder *result = addService("clipboard_bridge", binder);
    if (result) {
        LOGD("clipboard_bridge registered");
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

    void postServerSpecialize(const ServerSpecializeArgs *args) override {
        (void)args;
        LOGD("system_server post-specialize — registering clipboard_bridge");
        registerBridgeService();
    }
};

REGISTER_ZYGISK_MODULE(ClipSyncModule)
