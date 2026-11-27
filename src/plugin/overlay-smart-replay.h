#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void overlay_smart_replay_set_enabled(bool enabled);
bool overlay_smart_replay_is_enabled(void);
void overlay_smart_replay_set_mode(int mode); // 0 = timestamp trim, 1 = legacy restart
int overlay_smart_replay_get_mode(void);
void overlay_smart_replay_set_clip_ignore_trim(bool ignore);
bool overlay_smart_replay_get_clip_ignore_trim(void);

void overlay_smart_replay_reset_timestamps(void);
bool overlay_smart_replay_note_save_request(void);
void overlay_smart_replay_on_save_completed(const char *path, bool is_timed_clip);

#ifdef __cplusplus
}
#endif
