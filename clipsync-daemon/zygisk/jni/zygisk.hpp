/*
 * Official Zygisk API header — DO NOT MODIFY
 * From: https://github.com/topjohnwu/Magisk/blob/master/native/src/core/zygisk/api.hpp
 */
#pragma once

#include <jni.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>

#define ZYGISK_API_VERSION 5

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:
    virtual void onLoad(Api *api, JNIEnv *env) {}
    virtual void preAppSpecialize(AppSpecializeArgs *args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize(ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs *args) {}
    virtual ~ModuleBase() = default;
};

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jobjectArray &rlimits;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    jboolean *const is_child_zygote;
    jboolean *const is_top_app;
    jboolean *const mount_data_dirs;
    jboolean *const mount_storage_dirs;
    jboolean *const mount_sysprop_overrides;

    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ServerSpecializeArgs() = delete;
};

enum Option : int {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

enum StateFlag : uint32_t {
    PROCESS_GRANTED_ROOT = (1u << 0),
    PROCESS_ON_DENYLIST = (1u << 1),
};

namespace internal { struct api_table; }

struct Api {
    int connectCompanion() { return -1; }
    int getModuleDir() { return -1; }
    void setOption(Option opt) { (void)opt; }
    uint32_t getFlags() { return 0; }
    bool exemptFd(int fd) { (void)fd; return false; }
    void hookJniNativeMethods(JNIEnv *env, const char *className,
        JNINativeMethod *methods, int numMethods) {
        (void)env; (void)className; (void)methods; (void)numMethods;
    }
    void pltHookRegister(dev_t dev, ino_t inode, const char *symbol,
        void *newFunc, void **oldFunc) {
        (void)dev; (void)inode; (void)symbol; (void)newFunc; (void)oldFunc;
    }
    bool pltHookCommit() { return false; }

    internal::api_table *tbl = nullptr;
};

namespace internal {
struct api_table;

struct module_abi {
    long api_version;
    ModuleBase *impl;
    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

    module_abi(ModuleBase *module) : api_version(ZYGISK_API_VERSION), impl(module) {
        preAppSpecialize = [](auto m, auto args) { m->preAppSpecialize(args); };
        postAppSpecialize = [](auto m, auto args) { m->postAppSpecialize(args); };
        preServerSpecialize = [](auto m, auto args) { m->preServerSpecialize(args); };
        postServerSpecialize = [](auto m, auto args) { m->postServerSpecialize(args); };
    }
};

struct api_table {
    void *impl;
    bool (*registerModule)(api_table *, module_abi *);
};

template <class T>
void entry_impl(api_table *table, JNIEnv *env) {
    static Api api;
    api.tbl = table;
    static T module;
    ModuleBase *m = &module;
    static module_abi abi(m);
    if (!table->registerModule(table, &abi)) return;
    m->onLoad(&api, env);
}

} // namespace internal

} // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
    void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
        zygisk::internal::entry_impl<clazz>(table, env); \
    }

#define REGISTER_ZYGISK_COMPANION(func) \
    void zygisk_companion_entry(int client) { func(client); }
