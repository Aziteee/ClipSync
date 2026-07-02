// Minimal Zygisk API header for Zygisk Next
#pragma once

#include <jni.h>
#include <string>
#include <vector>

#define REGISTER_ZYGISK_MODULE(clazz) \
    extern "C" __attribute__((visibility("default"))) void zygisk_module_entry( \
        void *handle, void *api_ptr) { \
        auto *api = reinterpret_cast<zygisk::Api*>(api_ptr); \
        api->registerModule(new clazz()); \
    }

namespace zygisk {

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jint *&gids;
    jint &gids_len;
    jint &runtime_flags;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;
    jintArray &fds_to_ignore;
    jboolean &is_child_zygote;
    jboolean &is_top_app;
    jstring &pkg_data_dir;
    jstring *&allowed_paths;
    jstring *&visible_paths;
    jint &mount_mode;
    jboolean &mount_data_dirs;
    jboolean &mount_sysprop_overrides;
    jboolean &force_umount_sys;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &fds_to_ignore;
};

enum class Option {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

class Api {
public:
    virtual ~Api() = default;
    virtual void registerModule(void *module) = 0;
    virtual bool connectCompanion() = 0;
    virtual int getModuleDir() = 0;
    virtual int getModuleId() { return -1; }
    virtual void setOption(Option opt) = 0;
};

class ModuleBase {
public:
    virtual void onLoad(Api *api, JNIEnv *env) {}
    virtual void preAppSpecialize(AppSpecializeArgs *args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize(ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs *args) {}
    virtual ~ModuleBase() = default;
};

} // namespace zygisk
