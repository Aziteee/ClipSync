#ifndef BINDER_CLIP_H
#define BINDER_CLIP_H

typedef void (*clip_change_cb)(const char *text);

int  binder_clip_init(void);
char *binder_clip_get_text(void);
int  binder_clip_set_text(const char *text);
void binder_clip_set_callback(clip_change_cb cb);

#endif
