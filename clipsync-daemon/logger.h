#ifndef CLIPSYNC_LOGGER_H
#define CLIPSYNC_LOGGER_H

#define CLIPSYNC_LOG_PATH "/data/adb/clipsyncd/clipsyncd.log"

int logger_init(void);
void logger_close(void);
void log_printf(const char *fmt, ...);

#endif
