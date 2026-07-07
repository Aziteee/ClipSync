#ifndef CLIPSYNC_DAEMON_CONFIG_H
#define CLIPSYNC_DAEMON_CONFIG_H

#define CLIPSYNC_DEFAULT_CONFIG_PATH "/data/adb/modules/clipsyncd/config/clipsync.toml"

typedef struct {
    int port;
    char secret[256];
    int mdns_announce_interval_ms;
    char config_path[256];
    int config_loaded;
} clipsync_daemon_config;

void clipsync_config_init(clipsync_daemon_config *cfg);
int clipsync_config_load_file(clipsync_daemon_config *cfg, const char *path);
int clipsync_config_load_from_args(clipsync_daemon_config *cfg, int argc, char *argv[]);

#endif
