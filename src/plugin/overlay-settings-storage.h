#pragma once

#include <obs-data.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ensure_settings_config_dir(void);
obs_data_t *overlay_settings_get(void);
obs_data_t *overlay_settings_load(void);
bool overlay_settings_save(void);
void overlay_settings_free(void);

#ifdef __cplusplus
}
#endif
