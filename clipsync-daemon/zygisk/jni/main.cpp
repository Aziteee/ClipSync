/* ClipSync Zygisk Module — clipboard access bridge via Unix socket
 *
 * Architecture:
 *   Zygisk injects into system_server
 *   → Creates Unix domain socket @clipbridge (abstract namespace)
 *   → Accepts length-prefixed commands: "READ\n", "WRITE <len>\n<body>", "HAS\n"
 *   → Uses JNI to call local ClipboardService (no Binder, no permission check)
 *   → clipsyncd connects to the socket to read/write clipboard
 *
 * Zygisk lifecycle (critical):
 *   onLoad              : called after zygote forks the target child process
 *   preServerSpecialize : called before the child becomes system_server
 *   postServerSpecialize: called AFTER this process has become system_server
 * Therefore the socket MUST be started in postServerSpecialize, and the
 * socket thread must AttachCurrentThread because JNIEnv is thread-local.
 */
#include "zygisk.hpp"
#include "../../bridge_protocol.h"
#include <jni.h>
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <vector>
#include <cstdarg>

#define TAG "ClipSyncBridge"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef CLIPSYNC_BRIDGE_FILE_LOG
#define CLIPSYNC_BRIDGE_FILE_LOG 0
#endif

#include <fcntl.h>

/* Abstract Unix socket: no filesystem path, no init/SELinux path labels.
 * sun_path[0] == '\0', actual name follows. */
#define SOCK_ABSTRACT_NAME "clipbridge"
static constexpr const char *kClipboardCallerPackage = "android";

static constexpr const char *kHelperBinaryClassName = "dev.clipsync.bridge.ClipSyncBridgeHelper";

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static JNIEnv *g_env = nullptr;
static JavaVM *g_vm = nullptr;
static Api *g_api = nullptr;
static pthread_mutex_t g_watchers_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<int> g_watchers;
static jobject g_clip_listener = nullptr;

static std::vector<uint8_t> g_moduleDex;
static constexpr const char *kModuleDexPath = "/data/adb/modules/clipsyncd/zygisk/clipsync-helper.dex";

#if CLIPSYNC_BRIDGE_FILE_LOG
static void file_log(const char *fmt, ...) {
    int fd = open("/data/system/clipsync_bridge.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        write(fd, buf, len);
        write(fd, "\n", 1);
    }
    close(fd);
}
#else
static void file_log(const char *, ...) {}
#endif

static bool jni_clear_exception(const char *ctx) {
    if (!g_env || !g_env->ExceptionCheck()) return false;
    jthrowable ex = g_env->ExceptionOccurred();
    g_env->ExceptionClear();
    if (ex) {
        jclass exClass = g_env->GetObjectClass(ex);
        jmethodID toString = exClass ? g_env->GetMethodID(exClass, "toString", "()Ljava/lang/String;") : nullptr;
        jstring msg = toString ? (jstring)g_env->CallObjectMethod(ex, toString) : nullptr;
        const char *chars = msg ? g_env->GetStringUTFChars(msg, nullptr) : nullptr;
        file_log("%s: JNI exception: %s", ctx, chars ? chars : "(toString unavailable)");
        if (msg && chars) g_env->ReleaseStringUTFChars(msg, chars);
        if (msg) g_env->DeleteLocalRef(msg);
        if (exClass) g_env->DeleteLocalRef(exClass);
        g_env->DeleteLocalRef(ex);
    } else {
        file_log("%s: JNI exception", ctx);
    }
    LOGE("%s: JNI exception", ctx);
    return true;
}

static bool jni_drop_exception(void) {
    if (!g_env || !g_env->ExceptionCheck()) return false;
    g_env->ExceptionClear();
    return true;
}

static bool jni_ok(const char *ctx) {
    return !jni_clear_exception(ctx);
}

static jclass find_class(const char *name) {
    jclass cls = g_env ? g_env->FindClass(name) : nullptr;
    if (!cls || !jni_ok(name)) {
        LOGE("FindClass failed: %s", name);
        return nullptr;
    }
    return cls;
}

static jmethodID get_method(jclass cls, const char *name, const char *sig) {
    if (!cls) return nullptr;
    jmethodID method = g_env->GetMethodID(cls, name, sig);
    if (!method || !jni_ok(name)) {
        LOGE("GetMethodID failed: %s %s", name, sig);
        return nullptr;
    }
    return method;
}

static jmethodID get_static_method(jclass cls, const char *name, const char *sig) {
    if (!cls) return nullptr;
    jmethodID method = g_env->GetStaticMethodID(cls, name, sig);
    if (!method || !jni_ok(name)) {
        LOGE("GetStaticMethodID failed: %s %s", name, sig);
        return nullptr;
    }
    return method;
}

static jfieldID get_field(jclass cls, const char *name, const char *sig) {
    if (!cls) return nullptr;
    jfieldID field = g_env->GetFieldID(cls, name, sig);
    if (!field || !jni_ok(name)) {
        LOGE("GetFieldID failed: %s %s", name, sig);
        return nullptr;
    }
    return field;
}

static void add_watcher(int fd) {
    pthread_mutex_lock(&g_watchers_mutex);
    g_watchers.push_back(fd);
    pthread_mutex_unlock(&g_watchers_mutex);
}

static void notify_watchers(void) {
    pthread_mutex_lock(&g_watchers_mutex);
    file_log("notify_watchers: count=%lu", (unsigned long)g_watchers.size());
    for (auto it = g_watchers.begin(); it != g_watchers.end();) {
        int fd = *it;
        ssize_t n = send(fd, "CHANGED\n", 8, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            ++it;
        } else if (n < 0 || n != 8) {
            close(fd);
            it = g_watchers.erase(it);
        } else {
            ++it;
        }
    }
    pthread_mutex_unlock(&g_watchers_mutex);
}

static void companion_handler(int fd) {
    int dex_fd = open(kModuleDexPath, O_RDONLY);
    if (dex_fd < 0) {
        uint32_t size = 0;
        write(fd, &size, sizeof(size));
        return;
    }

    off_t end = lseek(dex_fd, 0, SEEK_END);
    lseek(dex_fd, 0, SEEK_SET);
    if (end <= 0 || (uint64_t)end > UINT32_MAX) {
        uint32_t size = 0;
        write(fd, &size, sizeof(size));
        close(dex_fd);
        return;
    }

    uint32_t size = (uint32_t)end;
    write(fd, &size, sizeof(size));

    char buf[4096];
    uint32_t remain = size;
    while (remain > 0) {
        size_t chunk = remain < sizeof(buf) ? (size_t)remain : sizeof(buf);
        ssize_t n = read(dex_fd, buf, chunk);
        if (n <= 0) break;
        write(fd, buf, (size_t)n);
        remain -= (uint32_t)n;
    }
    close(dex_fd);
}
REGISTER_ZYGISK_COMPANION(companion_handler)

extern "C" JNIEXPORT void JNICALL
Java_dev_clipsync_bridge_ClipSyncBridgeHelper_nativeOnClipboardChanged(JNIEnv *, jclass) {
    file_log("nativeOnClipboardChanged");
    notify_watchers();
}

static bool register_native_callback(jclass helperClass) {
    if (!helperClass || !g_env) return false;
    JNINativeMethod methods[] = {
        {"nativeOnClipboardChanged", "()V", (void *)Java_dev_clipsync_bridge_ClipSyncBridgeHelper_nativeOnClipboardChanged},
    };
    if (g_env->RegisterNatives(helperClass, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) {
        jni_clear_exception("RegisterNatives ClipSyncBridgeHelper");
        return false;
    }
    return true;
}

static jclass load_helper_class() {
    if (!g_env || g_moduleDex.empty()) {
        file_log("load_helper_class: no DEX bytes");
        return nullptr;
    }

    file_log("load_helper_class: start size=%lu", (unsigned long)g_moduleDex.size());

    jclass dexClClass = find_class("dalvik/system/InMemoryDexClassLoader");
    if (!dexClClass) {
        file_log("load_helper_class: InMemoryDexClassLoader class missing");
        return nullptr;
    }

    jmethodID dexClInit = g_env->GetMethodID(dexClClass, "<init>",
        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!dexClInit || !jni_ok("InMemoryDexClassLoader.<init>")) {
        file_log("load_helper_class: InMemoryDexClassLoader ctor missing");
        g_env->DeleteLocalRef(dexClClass);
        return nullptr;
    }

    jobject buf = g_env->NewDirectByteBuffer(g_moduleDex.data(), (jlong)g_moduleDex.size());
    if (!buf || !jni_ok("NewDirectByteBuffer")) {
        file_log("load_helper_class: NewDirectByteBuffer failed");
        if (buf) g_env->DeleteLocalRef(buf);
        g_env->DeleteLocalRef(dexClClass);
        return nullptr;
    }

    jclass baseLoaderClass = find_class("java/lang/ClassLoader");
    jmethodID getSystemClassLoader = baseLoaderClass
        ? get_static_method(baseLoaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;")
        : nullptr;
    jobject parent = getSystemClassLoader
        ? g_env->CallStaticObjectMethod(baseLoaderClass, getSystemClassLoader)
        : nullptr;
    if (!parent || !jni_ok("ClassLoader.getSystemClassLoader")) {
        parent = nullptr;
    }
    file_log("load_helper_class: parent=%s", parent ? "ok" : "null");

    jobject dexCl = g_env->NewObject(dexClClass, dexClInit, buf, parent);
    if (!dexCl || !jni_ok("InMemoryDexClassLoader.new")) {
        file_log("load_helper_class: InMemoryDexClassLoader.new failed");
        if (dexCl) g_env->DeleteLocalRef(dexCl);
        g_env->DeleteLocalRef(buf);
        if (parent) g_env->DeleteLocalRef(parent);
        if (baseLoaderClass) g_env->DeleteLocalRef(baseLoaderClass);
        g_env->DeleteLocalRef(dexClClass);
        return nullptr;
    }

    jclass classClass = find_class("java/lang/Class");
    jmethodID forName = classClass
        ? get_static_method(classClass, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;")
        : nullptr;
    if (!forName) {
        file_log("load_helper_class: Class.forName unavailable");
        g_env->DeleteLocalRef(dexCl);
        g_env->DeleteLocalRef(buf);
        if (parent) g_env->DeleteLocalRef(parent);
        if (baseLoaderClass) g_env->DeleteLocalRef(baseLoaderClass);
        g_env->DeleteLocalRef(dexClClass);
        if (classClass) g_env->DeleteLocalRef(classClass);
        return nullptr;
    }

    jstring name = g_env->NewStringUTF(kHelperBinaryClassName);
    jclass helper = name
        ? (jclass)g_env->CallStaticObjectMethod(classClass, forName, name, JNI_TRUE, dexCl)
        : nullptr;
    bool loaded = jni_ok("Class.forName ClipSyncBridgeHelper");
    if (!helper || !loaded) {
        file_log("load_helper_class: Class.forName failed helper=%p loaded=%d", helper, loaded ? 1 : 0);
        helper = nullptr;
    } else {
        file_log("load_helper_class: helper loaded via InMemoryDexClassLoader");
    }

    if (name) g_env->DeleteLocalRef(name);
    g_env->DeleteLocalRef(dexCl);
    g_env->DeleteLocalRef(buf);
    if (parent) g_env->DeleteLocalRef(parent);
    if (baseLoaderClass) g_env->DeleteLocalRef(baseLoaderClass);
    g_env->DeleteLocalRef(classClass);
    g_env->DeleteLocalRef(dexClClass);
    return helper;
}

/* --- JNI: Call local ClipboardService --- */
static jobject getClipboardService() {
    if (!g_env) return nullptr;
    jclass sm = find_class("android/os/ServiceManager");
    if (!sm) return nullptr;
    jmethodID getService = get_static_method(sm, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (!getService) { g_env->DeleteLocalRef(sm); return nullptr; }
    jstring name = g_env->NewStringUTF("clipboard");
    if (!name || !jni_ok("NewStringUTF clipboard")) { g_env->DeleteLocalRef(sm); return nullptr; }
    jobject binder = g_env->CallStaticObjectMethod(sm, getService, name);
    if (!jni_ok("ServiceManager.getService")) binder = nullptr;
    g_env->DeleteLocalRef(name); g_env->DeleteLocalRef(sm);
    if (!binder) return nullptr;
    jclass stub = find_class("android/content/IClipboard$Stub");
    if (!stub) { g_env->DeleteLocalRef(binder); return nullptr; }
    jmethodID asInterface = get_static_method(stub, "asInterface", "(Landroid/os/IBinder;)Landroid/content/IClipboard;");
    if (!asInterface) { g_env->DeleteLocalRef(binder); g_env->DeleteLocalRef(stub); return nullptr; }
    jobject cb = g_env->CallStaticObjectMethod(stub, asInterface, binder);
    if (!jni_ok("IClipboard.Stub.asInterface")) cb = nullptr;
    g_env->DeleteLocalRef(binder); g_env->DeleteLocalRef(stub);
    return cb;
}

static bool register_clipboard_listener(void) {
    if (g_clip_listener) return true;

    jclass helper = load_helper_class();
    if (!helper) {
        LOGE("clipboard listener helper not loaded (InMemoryDexClassLoader)");
        file_log("register_clipboard_listener: helper not loaded");
        return false;
    }
    if (!register_native_callback(helper)) {
        file_log("register_clipboard_listener: RegisterNatives failed");
        g_env->DeleteLocalRef(helper);
        return false;
    }

    jmethodID makeListener = get_static_method(helper, "makeListener", "()Landroid/content/IOnPrimaryClipChangedListener;");
    if (!makeListener) {
        file_log("register_clipboard_listener: makeListener missing");
        g_env->DeleteLocalRef(helper);
        return false;
    }

    jobject listener = g_env->CallStaticObjectMethod(helper, makeListener);
    if (!jni_ok("ClipSyncBridgeHelper.makeListener")) listener = nullptr;
    if (!listener) {
        LOGE("clipboard listener creation failed");
        file_log("register_clipboard_listener: listener creation failed");
        g_env->DeleteLocalRef(helper);
        return false;
    }

    jobject svc = getClipboardService();
    if (!svc) {
        g_env->DeleteLocalRef(listener);
        g_env->DeleteLocalRef(helper);
        LOGE("register listener: service null");
        file_log("register_clipboard_listener: service null");
        return false;
    }
    jclass iclip = g_env->GetObjectClass(svc);
    if (!iclip || !jni_ok("GetObjectClass IClipboard listener")) {
        if (iclip) g_env->DeleteLocalRef(iclip);
        g_env->DeleteLocalRef(svc);
        g_env->DeleteLocalRef(listener);
        g_env->DeleteLocalRef(helper);
        return false;
    }

    jstring pkg = g_env->NewStringUTF(kClipboardCallerPackage);
    if (!pkg || !jni_ok("NewStringUTF listener package")) {
        if (pkg) g_env->DeleteLocalRef(pkg);
        g_env->DeleteLocalRef(iclip);
        g_env->DeleteLocalRef(svc);
        g_env->DeleteLocalRef(listener);
        g_env->DeleteLocalRef(helper);
        return false;
    }

    bool public_ok = false;
    jmethodID add = g_env->GetMethodID(iclip, "addPrimaryClipChangedListener",
        "(Landroid/content/IOnPrimaryClipChangedListener;Ljava/lang/String;Ljava/lang/String;II)V");
    if (add) {
        g_env->CallVoidMethod(svc, add, listener, pkg, nullptr, (jint)0, (jint)0);
        public_ok = jni_ok("IClipboard.addPrimaryClipChangedListener modern");
    } else {
        jni_clear_exception("addPrimaryClipChangedListener modern lookup");
    }

    if (!public_ok) {
        add = g_env->GetMethodID(iclip, "addPrimaryClipChangedListener",
            "(Landroid/content/IOnPrimaryClipChangedListener;Ljava/lang/String;I)V");
        if (add) {
            g_env->CallVoidMethod(svc, add, listener, pkg, (jint)0);
            public_ok = jni_ok("IClipboard.addPrimaryClipChangedListener legacy");
        } else {
            jni_clear_exception("addPrimaryClipChangedListener legacy lookup");
        }
    }

    bool direct_ok = false;
    jmethodID direct = get_static_method(helper, "registerDirect",
        "(Ljava/lang/Object;Landroid/content/IOnPrimaryClipChangedListener;II)Z");
    if (direct) {
        direct_ok = g_env->CallStaticBooleanMethod(helper, direct, svc, listener, (jint)0, (jint)0) == JNI_TRUE;
        if (!jni_ok("ClipSyncBridgeHelper.registerDirect")) {
            direct_ok = false;
        }
    }

    bool ok = direct_ok || public_ok;
    if (ok) {
        g_clip_listener = g_env->NewGlobalRef(listener);
        if (!g_clip_listener || !jni_ok("NewGlobalRef clipboard listener")) {
            ok = false;
        }
    }

    g_env->DeleteLocalRef(pkg);
    g_env->DeleteLocalRef(iclip);
    g_env->DeleteLocalRef(svc);
    g_env->DeleteLocalRef(listener);
    g_env->DeleteLocalRef(helper);

    if (ok) {
        LOGD("clipboard listener registered");
        file_log("register_clipboard_listener: registered public=%d direct=%d",
                 public_ok ? 1 : 0, direct_ok ? 1 : 0);
    } else {
        LOGE("clipboard listener registration failed");
        file_log("register_clipboard_listener: registration failed");
    }
    return ok;
}

static char *clipDataToText(jobject cd) {
    if (!cd) return nullptr;
    char *result = nullptr;
    jclass cdClass = find_class("android/content/ClipData");
    if (!cdClass) return nullptr;
    jmethodID getItemAt = get_method(cdClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    if (!getItemAt) { g_env->DeleteLocalRef(cdClass); return nullptr; }
    jobject item = g_env->CallObjectMethod(cd, getItemAt, (jint)0);
    if (!jni_ok("ClipData.getItemAt")) item = nullptr;
    if (item) {
        jclass itemClass = g_env->GetObjectClass(item);
        if (!itemClass || !jni_ok("GetObjectClass ClipData.Item")) {
            if (itemClass) g_env->DeleteLocalRef(itemClass);
            g_env->DeleteLocalRef(item);
            g_env->DeleteLocalRef(cdClass);
            return nullptr;
        }
        jmethodID getText = get_method(itemClass, "getText", "()Ljava/lang/CharSequence;");
        if (!getText) { g_env->DeleteLocalRef(itemClass); g_env->DeleteLocalRef(item); g_env->DeleteLocalRef(cdClass); return nullptr; }
        jobject cs = g_env->CallObjectMethod(item, getText);
        if (!jni_ok("ClipData.Item.getText")) cs = nullptr;
        if (cs) {
            jclass csClass = g_env->GetObjectClass(cs);
            if (!csClass || !jni_ok("GetObjectClass CharSequence")) {
                if (csClass) g_env->DeleteLocalRef(csClass);
                g_env->DeleteLocalRef(cs);
                g_env->DeleteLocalRef(itemClass);
                g_env->DeleteLocalRef(item);
                g_env->DeleteLocalRef(cdClass);
                return nullptr;
            }
            jmethodID toString = get_method(csClass, "toString", "()Ljava/lang/String;");
            if (!toString) { g_env->DeleteLocalRef(csClass); g_env->DeleteLocalRef(cs); g_env->DeleteLocalRef(itemClass); g_env->DeleteLocalRef(item); g_env->DeleteLocalRef(cdClass); return nullptr; }
            jstring js = (jstring)g_env->CallObjectMethod(cs, toString);
            if (!jni_ok("CharSequence.toString")) js = nullptr;
            if (js) {
                const char *u = g_env->GetStringUTFChars(js, nullptr);
                if (u && jni_ok("GetStringUTFChars")) {
                    result = strdup(u);
                    g_env->ReleaseStringUTFChars(js, u);
                }
                g_env->DeleteLocalRef(js);
            }
            g_env->DeleteLocalRef(csClass);
            g_env->DeleteLocalRef(cs);
        }
        g_env->DeleteLocalRef(itemClass);
        g_env->DeleteLocalRef(item);
    }
    g_env->DeleteLocalRef(cdClass);
    return result;
}

static void logClassFields(jclass cls, const char *prefix) {
    jclass classClass = g_env->FindClass("java/lang/Class");
    jmethodID getName = g_env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");
    jmethodID getDeclaredFields = g_env->GetMethodID(classClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID getSuperclass = g_env->GetMethodID(classClass, "getSuperclass", "()Ljava/lang/Class;");
    jclass fieldClass = g_env->FindClass("java/lang/reflect/Field");
    jmethodID fieldGetName = g_env->GetMethodID(fieldClass, "getName", "()Ljava/lang/String;");
    jmethodID fieldGetType = g_env->GetMethodID(fieldClass, "getType", "()Ljava/lang/Class;");

    for (int depth = 0; cls && depth < 4; depth++) {
        jstring className = (jstring)g_env->CallObjectMethod(cls, getName);
        const char *classNameChars = className ? g_env->GetStringUTFChars(className, nullptr) : nullptr;
        LOGD("%s class[%d]=%s", prefix, depth, classNameChars ? classNameChars : "(null)");
        if (className && classNameChars) g_env->ReleaseStringUTFChars(className, classNameChars);
        if (className) g_env->DeleteLocalRef(className);

        jobjectArray fields = (jobjectArray)g_env->CallObjectMethod(cls, getDeclaredFields);
        if (fields && !g_env->ExceptionCheck()) {
            jsize count = g_env->GetArrayLength(fields);
            for (jsize i = 0; i < count && i < 80; i++) {
                jobject field = g_env->GetObjectArrayElement(fields, i);
                jstring fieldName = (jstring)g_env->CallObjectMethod(field, fieldGetName);
                jclass fieldType = (jclass)g_env->CallObjectMethod(field, fieldGetType);
                jstring fieldTypeName = fieldType ? (jstring)g_env->CallObjectMethod(fieldType, getName) : nullptr;
                const char *fieldNameChars = fieldName ? g_env->GetStringUTFChars(fieldName, nullptr) : nullptr;
                const char *fieldTypeChars = fieldTypeName ? g_env->GetStringUTFChars(fieldTypeName, nullptr) : nullptr;
                LOGD("%s field[%d]=%s type=%s", prefix, (int)i, fieldNameChars ? fieldNameChars : "(null)", fieldTypeChars ? fieldTypeChars : "(null)");
                if (fieldTypeName && fieldTypeChars) g_env->ReleaseStringUTFChars(fieldTypeName, fieldTypeChars);
                if (fieldTypeName) g_env->DeleteLocalRef(fieldTypeName);
                if (fieldType) g_env->DeleteLocalRef(fieldType);
                if (fieldName && fieldNameChars) g_env->ReleaseStringUTFChars(fieldName, fieldNameChars);
                if (fieldName) g_env->DeleteLocalRef(fieldName);
                g_env->DeleteLocalRef(field);
            }
            g_env->DeleteLocalRef(fields);
        } else if (g_env->ExceptionCheck()) {
            g_env->ExceptionClear();
        }

        jclass superCls = (jclass)g_env->CallObjectMethod(cls, getSuperclass);
        if (depth > 0) g_env->DeleteLocalRef(cls);
        cls = superCls;
    }
    if (cls) g_env->DeleteLocalRef(cls);
    g_env->DeleteLocalRef(fieldClass);
    g_env->DeleteLocalRef(classClass);
}

static jobject readClipboardDirect(jobject impl) {
    if (!impl) return nullptr;
    jclass implClass = g_env->GetObjectClass(impl);
    if (!implClass || !jni_ok("GetObjectClass IClipboard impl")) return nullptr;
    jfieldID outerField = get_field(implClass, "this$0", "Lcom/android/server/clipboard/ClipboardService;");
    if (!outerField) {
        g_env->DeleteLocalRef(implClass);
        LOGE("readClipboardDirect: ClipboardImpl.this$0 not found");
        return nullptr;
    }

    jobject service = g_env->GetObjectField(impl, outerField);
    if (!jni_ok("ClipboardImpl.this$0")) service = nullptr;
    g_env->DeleteLocalRef(implClass);
    if (!service) {
        LOGE("readClipboardDirect: ClipboardService outer is null");
        return nullptr;
    }

    jclass serviceClass = g_env->GetObjectClass(service);
    if (!serviceClass || !jni_ok("GetObjectClass ClipboardService")) {
        if (serviceClass) g_env->DeleteLocalRef(serviceClass);
        g_env->DeleteLocalRef(service);
        return nullptr;
    }

    jfieldID clipboardsField = g_env->GetFieldID(serviceClass, "mClipboards", "Landroid/util/SparseArray;");
    bool useSparseArray = (clipboardsField != nullptr);
    if (!useSparseArray) {
        jni_drop_exception();
        clipboardsField = g_env->GetFieldID(serviceClass, "mClipboards", "Landroid/util/SparseArrayMap;");
    }
    if (!clipboardsField) {
        jni_clear_exception("mClipboards SparseArrayMap probe");
        logClassFields(serviceClass, "ClipboardService");
        g_env->DeleteLocalRef(serviceClass);
        g_env->DeleteLocalRef(service);
        LOGE("readClipboardDirect: mClipboards not found (neither SparseArray nor SparseArrayMap)");
        return nullptr;
    }

    jobject clipboards = g_env->GetObjectField(service, clipboardsField);
    if (!jni_ok("ClipboardService.mClipboards")) clipboards = nullptr;
    g_env->DeleteLocalRef(serviceClass);
    g_env->DeleteLocalRef(service);
    if (!clipboards) {
        LOGE("readClipboardDirect: mClipboards is null");
        return nullptr;
    }

    jobject perUser = nullptr;
    if (useSparseArray) {
        jclass saClass = find_class("android/util/SparseArray");
        if (saClass) {
            jmethodID getMethod = g_env->GetMethodID(saClass, "get", "(I)Ljava/lang/Object;");
            if (!getMethod) jni_clear_exception("SparseArray.get");
            perUser = getMethod ? g_env->CallObjectMethod(clipboards, getMethod, (jint)0) : nullptr;
            if (!jni_ok("SparseArray.get")) perUser = nullptr;
            g_env->DeleteLocalRef(saClass);
        }
    } else {
        jclass integerClass = find_class("java/lang/Integer");
        if (integerClass) {
            jmethodID valueOf = get_static_method(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
            if (valueOf) {
                jobject deviceId = g_env->CallStaticObjectMethod(integerClass, valueOf, (jint)0);
                if (jni_ok("Integer.valueOf")) {
                    jclass smClass = find_class("android/util/SparseArrayMap");
                    if (smClass) {
                        jmethodID getMethod = g_env->GetMethodID(smClass, "get", "(ILjava/lang/Object;)Ljava/lang/Object;");
                        if (!getMethod) jni_clear_exception("SparseArrayMap.get");
                        perUser = getMethod ? g_env->CallObjectMethod(clipboards, getMethod, (jint)0, deviceId) : nullptr;
                        if (!jni_ok("SparseArrayMap.get")) perUser = nullptr;
                        g_env->DeleteLocalRef(smClass);
                    }
                }
                g_env->DeleteLocalRef(deviceId);
            }
            g_env->DeleteLocalRef(integerClass);
        }
    }

    g_env->DeleteLocalRef(clipboards);
    if (!perUser) {
        LOGE("readClipboardDirect: user 0 clipboard state is null");
        return nullptr;
    }

    jclass perUserClass = g_env->GetObjectClass(perUser);
    if (!perUserClass || !jni_ok("GetObjectClass perUser clipboard")) {
        if (perUserClass) g_env->DeleteLocalRef(perUserClass);
        g_env->DeleteLocalRef(perUser);
        return nullptr;
    }
    jfieldID primaryClipField = g_env->GetFieldID(perUserClass, "primaryClip", "Landroid/content/ClipData;");
    if (!primaryClipField) {
        jni_clear_exception("primaryClip probe");
        logClassFields(perUserClass, "PerUserState");
        g_env->DeleteLocalRef(perUserClass);
        g_env->DeleteLocalRef(perUser);
        LOGE("readClipboardDirect: primaryClip not found");
        return nullptr;
    }

    jobject clip = g_env->GetObjectField(perUser, primaryClipField);
    if (!jni_ok("ClipboardState.primaryClip")) clip = nullptr;
    g_env->DeleteLocalRef(perUserClass);
    g_env->DeleteLocalRef(perUser);
    return clip;
}

static char *readClipboard() {
    jobject svc = getClipboardService();
    if (!svc) { LOGE("readClipboard: service null"); return nullptr; }
    jclass iclip = g_env->GetObjectClass(svc);
    if (!iclip || !jni_ok("GetObjectClass IClipboard")) {
        if (iclip) g_env->DeleteLocalRef(iclip);
        g_env->DeleteLocalRef(svc);
        return nullptr;
    }
    bool modern_api = true;
    jmethodID m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;Ljava/lang/String;II)Landroid/content/ClipData;");
    if (!m) {
        jni_clear_exception("getPrimaryClip modern lookup");
        modern_api = false;
        m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;I)Landroid/content/ClipData;");
        if (!m) {
            jni_clear_exception("getPrimaryClip legacy lookup");
        }
    }
    jobject cd = nullptr;
    if (m) {
        jstring pkg = g_env->NewStringUTF(kClipboardCallerPackage);
        if (pkg && jni_ok("NewStringUTF read package")) {
            cd = modern_api
                ? g_env->CallObjectMethod(svc, m, pkg, nullptr, (jint)0, (jint)0)
                : g_env->CallObjectMethod(svc, m, pkg, (jint)0);
            if (!jni_ok("IClipboard.getPrimaryClip")) {
                cd = nullptr;
            }
        }
        if (pkg) g_env->DeleteLocalRef(pkg);
    }
    if (!cd) {
        cd = readClipboardDirect(svc);
    }

    char *result = clipDataToText(cd);
    if (cd) {
        g_env->DeleteLocalRef(cd);
    }
    g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
    return result;
}

static bool writeClipboard(const char *text) {
    if (!text) return false;
    jobject svc = getClipboardService();
    if (!svc) { LOGE("writeClipboard: service null"); return false; }
    jclass iclip = g_env->GetObjectClass(svc);
    if (!iclip || !jni_ok("GetObjectClass IClipboard write")) {
        if (iclip) g_env->DeleteLocalRef(iclip);
        g_env->DeleteLocalRef(svc);
        return false;
    }
    bool modern_api = true;
    jmethodID m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;Ljava/lang/String;II)V");
    if (!m) {
        jni_clear_exception("setPrimaryClip modern lookup");
        modern_api = false;
        m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;I)V");
        if (!m) {
            jni_clear_exception("setPrimaryClip legacy lookup");
        }
    }
    if (!m) { LOGE("writeClipboard: setPrimaryClip method not found"); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return false; }
    jclass cd = find_class("android/content/ClipData");
    if (!cd) { g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return false; }
    jmethodID npt = get_static_method(cd, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    if (!npt) { g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return false; }
    jstring lb = g_env->NewStringUTF("ClipSync");
    jstring tx = g_env->NewStringUTF(text);
    if (!lb || !tx || !jni_ok("NewStringUTF write text")) {
        if (lb) g_env->DeleteLocalRef(lb);
        if (tx) g_env->DeleteLocalRef(tx);
        g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
        return false;
    }
    jobject clip = g_env->CallStaticObjectMethod(cd, npt, lb, tx);
    if (!jni_ok("ClipData.newPlainText")) clip = nullptr;
    g_env->DeleteLocalRef(lb); g_env->DeleteLocalRef(tx);
    bool ok = false;
    if (clip) {
        jstring pk = g_env->NewStringUTF(kClipboardCallerPackage);
        if (pk && jni_ok("NewStringUTF write package")) {
            if (modern_api) {
                g_env->CallVoidMethod(svc, m, clip, pk, nullptr, (jint)0, (jint)0);
            } else {
                g_env->CallVoidMethod(svc, m, clip, pk, (jint)0);
            }
            ok = jni_ok("IClipboard.setPrimaryClip");
        }
        if (pk) g_env->DeleteLocalRef(pk);
        g_env->DeleteLocalRef(clip);
    } else {
        LOGE("writeClipboard: ClipData creation failed");
    }
    g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
    return ok;
}

/* --- Socket server --- */
static bool peer_is_root(int fd) {
    struct ucred cred = {};
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        LOGE("SO_PEERCRED failed: %s", strerror(errno));
        return false;
    }
    if (cred.uid != 0) {
        LOGE("rejecting @clipbridge peer pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
        return false;
    }
    return true;
}

static void write_err(int fd, const char *reason) {
    bridge_write_cstr(fd, "ERR ");
    bridge_write_cstr(fd, reason ? reason : "unknown");
    bridge_write_cstr(fd, "\n");
}

static void handle_client(int fd) {
    char line[256];

    if (!peer_is_root(fd)) {
        write_err(fd, "forbidden");
        close(fd);
        return;
    }

    if (bridge_read_line(fd, line, sizeof(line)) != 0) {
        write_err(fd, "bad_header");
        close(fd);
        return;
    }

    if (strcmp(line, "READ\n") == 0) {
        char *text = readClipboard();
        size_t len = text ? strlen(text) : 0;
        char header[64];
        if (len > CLIPSYNC_BRIDGE_MAX_PAYLOAD) {
            if (text) free(text);
            write_err(fd, "too_large");
            close(fd);
            return;
        }
        snprintf(header, sizeof(header), "DATA %lu\n", (unsigned long)len);
        bridge_write_cstr(fd, header);
        if (text) {
            bridge_write_full(fd, text, len);
            free(text);
        }
    } else if (strncmp(line, "WRITE ", 6) == 0) {
        size_t len = 0;
        char *text;
        if (bridge_parse_len_header(line, "WRITE ", &len) != 0) {
            write_err(fd, "bad_length");
            close(fd);
            return;
        }
        text = (char *)malloc(len + 1);
        if (!text) {
            write_err(fd, "oom");
            close(fd);
            return;
        }
        if (bridge_read_full(fd, text, len) != 0) {
            free(text);
            write_err(fd, "bad_body");
            close(fd);
            return;
        }
        text[len] = '\0';
        if (writeClipboard(text)) {
            bridge_write_cstr(fd, "OK\n");
        } else {
            write_err(fd, "write_failed");
        }
        free(text);
    } else if (strcmp(line, "HAS\n") == 0) {
        char *text = readClipboard();
        bridge_write_cstr(fd, text ? "HAS 1\n" : "HAS 0\n");
        if (text) free(text);
    } else if (strcmp(line, "WATCH\n") == 0) {
        int flags;
        if (!g_clip_listener) {
            register_clipboard_listener();
        }
        if (!g_clip_listener) {
            write_err(fd, "watch_unavailable");
            close(fd);
            return;
        }
        if (bridge_write_cstr(fd, "READY\n") != 0) {
            close(fd);
            return;
        }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        add_watcher(fd);
        LOGD("WATCH ready fd=%d", fd);
        return;
    } else {
        write_err(fd, "unknown_command");
    }
    close(fd);
}

static void *socket_thread(void *) {
    /* Attach this pthread to the JVM: JNIEnv is thread-local and the thread
     * that called onLoad/postServerSpecialize is different from this worker. */
    JNIEnv *env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("AttachCurrentThread failed");
        return nullptr;
    }
    g_env = env;
    LOGD("socket_thread attached to JVM");
    if (!register_clipboard_listener()) {
        LOGE("clipboard listener unavailable; WATCH clients will be rejected");
    }

    int fd = -1;
    struct sockaddr_un addr = {};
    socklen_t addr_len = 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket failed: %s", strerror(errno));
    } else {
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, SOCK_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);
        addr_len = (socklen_t)(1 + strlen(SOCK_ABSTRACT_NAME) + sizeof(sa_family_t));

        /* Abstract sockets do not leave filesystem entries; no unlink needed. */
        if (bind(fd, (struct sockaddr*)&addr, addr_len) < 0) {
            LOGE("bind failed: %s", strerror(errno));
            close(fd);
            fd = -1;
        } else {
            listen(fd, 5);
            LOGD("socket listening on @%s", SOCK_ABSTRACT_NAME);
            while (true) {
                int client = accept(fd, nullptr, nullptr);
                if (client < 0) break;
                handle_client(client);
            }
            close(fd);
        }
    }

    g_env = nullptr;
    g_vm->DetachCurrentThread();
    return nullptr;
}

class ClipSyncModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        g_api = api;
        g_env = env;
        if (env->GetJavaVM(&g_vm) != JNI_OK) g_vm = nullptr;
        /* Keep onLoad quiet: it also runs in ordinary app child processes. */
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        (void)args;
        if (g_api) {
            g_api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        (void)args;
        if (!g_api) return;

        int fd = g_api->connectCompanion();
        if (fd < 0) {
            LOGE("connectCompanion failed, DEX loading unavailable");
            return;
        }

        uint32_t size = 0;
        if (bridge_read_full(fd, &size, sizeof(size)) != 0 || size == 0) {
            LOGE("companion: invalid DEX size %u", size);
            close(fd);
            return;
        }

        g_moduleDex.resize(size);
        if (bridge_read_full(fd, g_moduleDex.data(), size) != 0) {
            LOGE("companion: failed to read DEX bytes");
            g_moduleDex.clear();
            close(fd);
            return;
        }

        close(fd);
        LOGD("companion: received %u bytes DEX", size);
    }

    void postServerSpecialize(const ServerSpecializeArgs *args) override {
        (void)args;
        pthread_t t;
        pthread_create(&t, nullptr, socket_thread, nullptr);
        pthread_detach(t);
    }
};

REGISTER_ZYGISK_MODULE(ClipSyncModule)
