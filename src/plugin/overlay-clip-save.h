#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int overlay_clip_save_get_buffer_length(void);
bool overlay_clip_save_request(int seconds);
bool overlay_clip_save_on_replay_saved(const char *path);
bool overlay_clip_save_is_pending(void);

#ifdef __cplusplus
}
#endif
