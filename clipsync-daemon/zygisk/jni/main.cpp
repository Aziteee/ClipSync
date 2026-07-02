/* ClipSync Zygisk Module — clipboard access bridge via Unix socket
 *
 * Architecture:
 *   Zygisk injects into system_server
 *   → Creates Unix domain socket @clipbridge (abstract namespace)
 *   → Accepts simple text commands: "READ\n", "WRITE text\n", "HAS\n"
 *   → Uses JNI to call local ClipboardService (no Binder, no permission check)
 *   → clipsyncd connects to the socket to read/write clipboard
 *
 * Zygisk lifecycle (critical):
 *   onLoad              : called in the zygote process (NOT system_server)
 *   preServerSpecialize : called in zygote BEFORE becoming system_server
 *   postServerSpecialize: called AFTER this process has become system_server
 * Therefore the socket MUST be started in postServerSpecialize, and the
 * socket thread must AttachCurrentThread because JNIEnv is thread-local.
 */
#include "zygisk.hpp"
#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define TAG "ClipSyncBridge"
#define LOG_PATH "/data/adb/modules/clipsyncd/bridge.log"

static void kmsg_log(const char *fmt, ...) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        write(fd, buf, n);
        write(fd, "\n", 1);
    }
    close(fd);
}

#define LOGD(...) do { \
    __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__); \
    kmsg_log(__VA_ARGS__); \
    FILE *f = fopen(LOG_PATH, "a"); \
    if (f) { fprintf(f, __VA_ARGS__); fprintf(f, "\n"); fclose(f); } \
} while(0)
#define LOGE(...) LOGD("ERROR: " __VA_ARGS__)

/* Abstract Unix socket: no filesystem path, no init/SELinux path labels.
 * sun_path[0] == '\0', actual name follows. */
#define SOCK_ABSTRACT_NAME "clipbridge"
static constexpr const char *kClipboardCallerPackage = "android";

using zygisk::ServerSpecializeArgs;
using zygisk::Api;

static JNIEnv *g_env = nullptr;
static JavaVM *g_vm = nullptr;

static void log_cmdline(const char *prefix) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) { LOGD("%s: cannot open /proc/self/cmdline", prefix); return; }
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
    LOGD("%s: cmdline=[%s]", prefix, buf);
}

/* --- JNI: Call local ClipboardService --- */
static jobject getClipboardService() {
    if (!g_env) return nullptr;
    jclass sm = g_env->FindClass("android/os/ServiceManager");
    if (!sm) { LOGE("ServiceManager class not found"); return nullptr; }
    jmethodID getService = g_env->GetStaticMethodID(sm, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (!getService) return nullptr;
    jstring name = g_env->NewStringUTF("clipboard");
    jobject binder = g_env->CallStaticObjectMethod(sm, getService, name);
    g_env->DeleteLocalRef(name); g_env->DeleteLocalRef(sm);
    if (!binder) return nullptr;
    jclass stub = g_env->FindClass("android/content/IClipboard$Stub");
    jmethodID asInterface = g_env->GetStaticMethodID(stub, "asInterface", "(Landroid/os/IBinder;)Landroid/content/IClipboard;");
    jobject cb = g_env->CallStaticObjectMethod(stub, asInterface, binder);
    g_env->DeleteLocalRef(binder); g_env->DeleteLocalRef(stub);
    return cb;
}

static char *clipDataToText(jobject cd) {
    if (!cd) return nullptr;
    char *result = nullptr;
    jclass cdClass = g_env->FindClass("android/content/ClipData");
    jmethodID getItemAt = g_env->GetMethodID(cdClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    jobject item = g_env->CallObjectMethod(cd, getItemAt, (jint)0);
    if (item) {
        jclass itemClass = g_env->GetObjectClass(item);
        jmethodID getText = g_env->GetMethodID(itemClass, "getText", "()Ljava/lang/CharSequence;");
        jobject cs = g_env->CallObjectMethod(item, getText);
        if (cs) {
            jclass csClass = g_env->GetObjectClass(cs);
            jmethodID toString = g_env->GetMethodID(csClass, "toString", "()Ljava/lang/String;");
            jstring js = (jstring)g_env->CallObjectMethod(cs, toString);
            if (js) {
                const char *u = g_env->GetStringUTFChars(js, nullptr);
                if (u) result = strdup(u);
                g_env->ReleaseStringUTFChars(js, u);
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
    jfieldID outerField = g_env->GetFieldID(implClass, "this$0", "Lcom/android/server/clipboard/ClipboardService;");
    if (!outerField || g_env->ExceptionCheck()) {
        g_env->ExceptionClear();
        g_env->DeleteLocalRef(implClass);
        LOGE("readClipboardDirect: ClipboardImpl.this$0 not found");
        return nullptr;
    }

    jobject service = g_env->GetObjectField(impl, outerField);
    g_env->DeleteLocalRef(implClass);
    if (!service) {
        LOGE("readClipboardDirect: ClipboardService outer is null");
        return nullptr;
    }

    jclass serviceClass = g_env->GetObjectClass(service);
    jfieldID clipboardsField = g_env->GetFieldID(serviceClass, "mClipboards", "Landroid/util/SparseArrayMap;");
    if (!clipboardsField || g_env->ExceptionCheck()) {
        g_env->ExceptionClear();
        logClassFields(serviceClass, "ClipboardService");
        g_env->DeleteLocalRef(serviceClass);
        g_env->DeleteLocalRef(service);
        LOGE("readClipboardDirect: mClipboards not found");
        return nullptr;
    }

    jobject clipboards = g_env->GetObjectField(service, clipboardsField);
    g_env->DeleteLocalRef(serviceClass);
    g_env->DeleteLocalRef(service);
    if (!clipboards) {
        LOGE("readClipboardDirect: mClipboards is null");
        return nullptr;
    }

    jclass integerClass = g_env->FindClass("java/lang/Integer");
    jmethodID valueOf = g_env->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    jobject deviceId = g_env->CallStaticObjectMethod(integerClass, valueOf, (jint)0);
    jclass sparseMapClass = g_env->FindClass("android/util/SparseArrayMap");
    jmethodID getMethod = g_env->GetMethodID(sparseMapClass, "get", "(ILjava/lang/Object;)Ljava/lang/Object;");
    jobject perUser = getMethod ? g_env->CallObjectMethod(clipboards, getMethod, (jint)0, deviceId) : nullptr;
    if (g_env->ExceptionCheck()) {
        g_env->ExceptionDescribe();
        g_env->ExceptionClear();
        perUser = nullptr;
    }
    g_env->DeleteLocalRef(sparseMapClass);
    g_env->DeleteLocalRef(deviceId);
    g_env->DeleteLocalRef(integerClass);
    g_env->DeleteLocalRef(clipboards);
    if (!perUser) {
        LOGE("readClipboardDirect: user 0 clipboard state is null");
        return nullptr;
    }

    jclass perUserClass = g_env->GetObjectClass(perUser);
    jfieldID primaryClipField = g_env->GetFieldID(perUserClass, "primaryClip", "Landroid/content/ClipData;");
    if (!primaryClipField || g_env->ExceptionCheck()) {
        g_env->ExceptionClear();
        g_env->DeleteLocalRef(perUserClass);
        g_env->DeleteLocalRef(perUser);
        LOGE("readClipboardDirect: primaryClip not found");
        return nullptr;
    }

    jobject clip = g_env->GetObjectField(perUser, primaryClipField);
    g_env->DeleteLocalRef(perUserClass);
    g_env->DeleteLocalRef(perUser);
    return clip;
}

static char *readClipboard() {
    jobject svc = getClipboardService();
    if (!svc) { LOGE("readClipboard: service null"); return nullptr; }
    jclass iclip = g_env->GetObjectClass(svc);
    bool modern_api = true;
    jmethodID m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;Ljava/lang/String;II)Landroid/content/ClipData;");
    if (!m) {
        g_env->ExceptionClear();
        modern_api = false;
        m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;I)Landroid/content/ClipData;");
    }
    if (!m) { LOGE("readClipboard: getPrimaryClip method not found"); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return nullptr; }
    jstring pkg = g_env->NewStringUTF(kClipboardCallerPackage);
    jobject cd = modern_api
        ? g_env->CallObjectMethod(svc, m, pkg, nullptr, (jint)0, (jint)0)
        : g_env->CallObjectMethod(svc, m, pkg, (jint)0);
    if (g_env->ExceptionCheck()) {
        g_env->ExceptionDescribe();
        g_env->ExceptionClear();
        cd = nullptr;
    }
    g_env->DeleteLocalRef(pkg);

    bool direct = false;
    if (!cd) {
        cd = readClipboardDirect(svc);
        direct = cd != nullptr;
    }

    char *result = clipDataToText(cd);
    if (cd) {
        g_env->DeleteLocalRef(cd);
    }
    if (direct && result) LOGD("readClipboard: direct fallback returned %zu chars", strlen(result));
    g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
    return result;
}

static void writeClipboard(const char *text) {
    if (!text) return;
    jobject svc = getClipboardService();
    if (!svc) { LOGE("writeClipboard: service null"); return; }
    jclass iclip = g_env->GetObjectClass(svc);
    bool modern_api = true;
    jmethodID m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;Ljava/lang/String;II)V");
    if (!m) {
        g_env->ExceptionClear();
        modern_api = false;
        m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;I)V");
    }
    if (!m) { LOGE("writeClipboard: setPrimaryClip method not found"); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return; }
    jclass cd = g_env->FindClass("android/content/ClipData");
    jmethodID npt = g_env->GetStaticMethodID(cd, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    jstring lb = g_env->NewStringUTF("ClipSync");
    jstring tx = g_env->NewStringUTF(text);
    jobject clip = g_env->CallStaticObjectMethod(cd, npt, lb, tx);
    g_env->DeleteLocalRef(lb); g_env->DeleteLocalRef(tx);
    if (clip) {
        jstring pk = g_env->NewStringUTF(kClipboardCallerPackage);
        if (modern_api) {
            g_env->CallVoidMethod(svc, m, clip, pk, nullptr, (jint)0, (jint)0);
        } else {
            g_env->CallVoidMethod(svc, m, clip, pk, (jint)0);
        }
        if (g_env->ExceptionCheck()) {
            g_env->ExceptionDescribe();
            g_env->ExceptionClear();
        }
        g_env->DeleteLocalRef(pk); g_env->DeleteLocalRef(clip);
    } else {
        LOGE("writeClipboard: ClipData creation failed");
    }
    g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
}

/* --- Socket server --- */
static void handle_client(int fd) {
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = 0;

    if (strncmp(buf, "READ", 4) == 0) {
        char *text = readClipboard();
        if (text) {
            write(fd, text, strlen(text));
            free(text);
        }
        write(fd, "\n", 1);
    } else if (strncmp(buf, "WRITE ", 6) == 0) {
        char *text = buf + 6;
        size_t len = strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
            text[--len] = '\0';
        }
        writeClipboard(text);
        write(fd, "OK\n", 3);
    } else if (strncmp(buf, "HAS", 3) == 0) {
        char *text = readClipboard();
        write(fd, text ? "1\n" : "0\n", 2);
        if (text) free(text);
    } else {
        write(fd, "ERR\n", 4);
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
        (void)api;
        g_env = env;
        if (env->GetJavaVM(&g_vm) != JNI_OK) {
            LOGE("GetJavaVM failed in onLoad");
        }
        LOGD("onLoad called");
        log_cmdline("onLoad");
        /* onLoad runs in zygote. Do NOT try to detect system_server here.
         * ActivityThread is not initialized and the process name is "zygote". */
    }

    void preServerSpecialize(ServerSpecializeArgs *args) override {
        (void)args;
        LOGD("preServerSpecialize called");
        log_cmdline("preServerSpecialize");
        /* Still in zygote here; do not access system_server services yet. */
    }

    void postServerSpecialize(const ServerSpecializeArgs *args) override {
        (void)args;
        LOGD("postServerSpecialize called");
        log_cmdline("postServerSpecialize");
        /* We are now running inside system_server. Start the bridge socket. */
        pthread_t t;
        pthread_create(&t, nullptr, socket_thread, nullptr);
        pthread_detach(t);
    }
};

REGISTER_ZYGISK_MODULE(ClipSyncModule)
