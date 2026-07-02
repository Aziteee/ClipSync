/* ClipSync Zygisk Module — clipboard access bridge via Unix socket
 *
 * Architecture:
 *   Zygisk injects into system_server
 *   → Creates Unix domain socket /data/local/tmp/clipbridge.sock
 *   → Accepts simple text commands: "READ\n", "WRITE text\n", "HAS\n"
 *   → Uses JNI to call local ClipboardService (no Binder, no permission check)
 *   → clipsyncd connects to the socket to read/write clipboard
 */
#include "zygisk.hpp"
#include <android/log.h>
#include <jni.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define TAG "ClipSyncBridge"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SOCK_PATH "/dev/socket/clipbridge"

using zygisk::ServerSpecializeArgs;
using zygisk::Api;

static JNIEnv *g_env = nullptr;

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

static char *readClipboard() {
    jobject svc = getClipboardService();
    if (!svc) return nullptr;
    jclass iclip = g_env->GetObjectClass(svc);
    jmethodID m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;Ljava/lang/String;II)Landroid/content/ClipData;");
    if (!m) m = g_env->GetMethodID(iclip, "getPrimaryClip", "(Ljava/lang/String;I)Landroid/content/ClipData;");
    if (!m) { g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return nullptr; }
    jstring pkg = g_env->NewStringUTF("com.android.shell");
    jobject cd = g_env->CallObjectMethod(svc, m, pkg, nullptr, (jint)0, (jint)0);
    g_env->DeleteLocalRef(pkg);
    char *result = nullptr;
    if (cd) {
        jclass cdClass = g_env->FindClass("android/content/ClipData");
        jmethodID getItemAt = g_env->GetMethodID(cdClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
        jobject item = g_env->CallObjectMethod(cd, getItemAt, (jint)0);
        if (item) {
            jclass itemClass = g_env->GetObjectClass(item);
            jmethodID getText = g_env->GetMethodID(itemClass, "getText", "()Ljava/lang/CharSequence;");
            jobject cs = g_env->CallObjectMethod(item, getText);
            if (cs) {
                jstring js = (jstring)g_env->CallObjectMethod(cs, g_env->GetMethodID(g_env->GetObjectClass(cs), "toString", "()Ljava/lang/String;"));
                if (js) {
                    const char *u = g_env->GetStringUTFChars(js, nullptr);
                    if (u) result = strdup(u);
                    g_env->ReleaseStringUTFChars(js, u);
                    g_env->DeleteLocalRef(js);
                }
                g_env->DeleteLocalRef(cs);
            }
            g_env->DeleteLocalRef(item); g_env->DeleteLocalRef(itemClass);
        }
        g_env->DeleteLocalRef(cd); g_env->DeleteLocalRef(cdClass);
    }
    g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc);
    return result;
}

static void writeClipboard(const char *text) {
    if (!text) return;
    jobject svc = getClipboardService();
    if (!svc) return;
    jclass iclip = g_env->GetObjectClass(svc);
    jmethodID m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;Ljava/lang/String;II)V");
    if (!m) {
        m = g_env->GetMethodID(iclip, "setPrimaryClip", "(Landroid/content/ClipData;Ljava/lang/String;I)V");
    }
    if (!m) { g_env->DeleteLocalRef(iclip); g_env->DeleteLocalRef(svc); return; }
    jclass cd = g_env->FindClass("android/content/ClipData");
    jmethodID npt = g_env->GetStaticMethodID(cd, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    jstring lb = g_env->NewStringUTF("ClipSync");
    jstring tx = g_env->NewStringUTF(text);
    jobject clip = g_env->CallStaticObjectMethod(cd, npt, lb, tx);
    g_env->DeleteLocalRef(lb); g_env->DeleteLocalRef(tx);
    if (clip) {
        jstring pk = g_env->NewStringUTF("com.android.shell");
        g_env->CallVoidMethod(svc, m, clip, pk, nullptr, (jint)0, (jint)0);
        g_env->DeleteLocalRef(pk); g_env->DeleteLocalRef(clip);
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
        writeClipboard(buf + 6);
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
    unlink(SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { LOGE("socket failed"); return nullptr; }
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("bind failed: %s", strerror(errno)); close(fd); return nullptr;
    }
    chmod(SOCK_PATH, 0666);
    listen(fd, 5);
    LOGD("socket listening on %s", SOCK_PATH);
    while (true) {
        int client = accept(fd, nullptr, nullptr);
        if (client < 0) break;
        // handle in thread
        pthread_t t;
        // Simple: handle inline for now
        handle_client(client);
    }
    close(fd);
    return nullptr;
}

class ClipSyncModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        (void)api;
        g_env = env;
        LOGD("ClipSync Zygisk module loaded");
    }
    void postServerSpecialize(const ServerSpecializeArgs *args) override {
        (void)args;
        LOGD("starting clipboard bridge socket server");
        pthread_t t;
        pthread_create(&t, nullptr, socket_thread, nullptr);
        pthread_detach(t);
    }
};

REGISTER_ZYGISK_MODULE(ClipSyncModule)
