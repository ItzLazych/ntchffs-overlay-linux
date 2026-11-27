#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool overlay_indicators_create(void);
void overlay_indicators_destroy(void);
void overlay_indicators_set_enabled(bool enabled);
void overlay_indicators_set_position(int pos);
void overlay_indicators_set_oled_protection(bool enabled);
void overlay_indicators_set_scale(int scale_percent);
void overlay_indicators_set_preview(bool preview);
void overlay_indicators_set_monitor(int mode, int index);
void overlay_indicators_set_custom_icon(int slot, const char *path, bool tint, uint32_t color_rgb);
void overlay_indicators_set_sound_path(int event_idx, const char *path);
bool overlay_indicators_play_sound(int event_idx);
void overlay_indicators_update_state(bool recording, bool replay_active, bool replay_saving);
void overlay_indicators_notify_replay_saved(void);

#ifdef __cplusplus
}
#endif
