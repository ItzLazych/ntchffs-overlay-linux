#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*overlay_toast_action_cb)(int action_kind, const char *file_path, void *userdata);

bool overlay_toasts_create(void);
void overlay_toasts_destroy(void);
void overlay_toasts_set_enabled(bool enabled);
void overlay_toasts_set_video_saved(bool enabled);
void overlay_toasts_set_export_enabled(bool enabled);
void overlay_toasts_set_open_hint(bool enabled);
void overlay_toasts_set_low_disk(bool enabled, int threshold_gb);
void overlay_toasts_set_duration_sec(int sec);
void overlay_toasts_set_accent_color(uint32_t color_rgb);
void overlay_toasts_set_hotkeys_labels(const char *view_label, const char *delete_label);
void overlay_toasts_set_monitor(int mode, int index);
void overlay_toasts_set_action_callback(overlay_toast_action_cb cb, void *userdata);

void overlay_toasts_show_replay_saved(const char *file_path, bool is_recording);
void overlay_toasts_show_export_result(bool success, bool merged, const char *detail_or_error);
void overlay_toasts_show_open_hint(const char *toggle_hotkey_label);
void overlay_toasts_check_low_disk(uint64_t free_bytes);
bool overlay_toasts_trigger_hotkey_action(int action_kind);

#ifdef __cplusplus
}
#endif
