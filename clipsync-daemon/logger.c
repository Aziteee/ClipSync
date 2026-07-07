#include "logger.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE *g_log_fp = NULL;

int logger_init(void) {
    g_log_fp = fopen(CLIPSYNC_LOG_PATH, "w");
    if (!g_log_fp) {
        fprintf(stderr, "[logger] failed to open %s\n", CLIPSYNC_LOG_PATH);
        return -1;
    }
    setvbuf(g_log_fp, NULL, _IOLBF, 0);
    return 0;
}

void logger_close(void) {
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

void log_printf(const char *fmt, ...) {
    FILE *fp;
    time_t now;
    struct tm tm_buf;
    char ts[32];
    va_list ap;

    fp = g_log_fp ? g_log_fp : stderr;

    now = time(NULL);
    localtime_r(&now, &tm_buf);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    flockfile(fp);
    fprintf(fp, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
    funlockfile(fp);
}
