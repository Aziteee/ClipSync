#ifndef CLIP_BRIDGE_CLIENT_H
#define CLIP_BRIDGE_CLIENT_H

int  clip_bridge_init(void);
char *clip_bridge_get_text(void);
int  clip_bridge_set_text(const char *text);
int  clip_bridge_watch_start(void (*notify_fn)(void *), void *notify_arg);
void clip_bridge_watch_stop(void);
int  clip_bridge_watch_take_changed(void);
void clip_bridge_set_action_callback(void (*fn)(void *, int), void *arg);

/* Post a notification via the bridge.
 * notif_id:    Android notification ID (reuse to replace)
 * title/text:  notification content (may be NULL)
 * num_actions: number of action buttons (0 for none)
 * labels:      button label strings (length == num_actions, may be NULL if 0)
 * action_ids:  IDs returned when button tapped (length == num_actions)
 * Returns 0 on success, -1 on error. */
int clip_bridge_post_notification(int notif_id, const char *title, const char *text,
        int num_actions, const char **labels, const int *action_ids);

#endif
