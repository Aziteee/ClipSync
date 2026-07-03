#ifndef CLIPSYNC_BRIDGE_PROTOCOL_H
#define CLIPSYNC_BRIDGE_PROTOCOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIPSYNC_BRIDGE_MAX_PAYLOAD 262144U

int bridge_read_line(int fd, char *buf, size_t buf_len);
int bridge_read_full(int fd, void *buf, size_t len);
int bridge_write_full(int fd, const void *buf, size_t len);
int bridge_write_cstr(int fd, const char *text);
int bridge_parse_len_header(const char *line, const char *prefix, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
