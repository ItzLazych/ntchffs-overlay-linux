#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool overlay_window_create(void);
void overlay_window_destroy(void);
void overlay_window_show(void);
void overlay_window_hide(void);
void overlay_window_toggle(void);
bool overlay_window_is_visible(void);

void overlay_window_open_settings(void);
void overlay_window_open_file_in_gallery(const char *file_path);
void overlay_window_release_file_if_open(const char *file_path);
void overlay_window_on_file_deleted(const char *file_path);
void overlay_window_notify_replay_saving(bool saving);
void overlay_window_reload_settings(void);

#ifdef __cplusplus
}
#endif
