#include "overlay-settings-storage.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "plugin-support.h"

static obs_data_t *g_saved_settings_data = nullptr;

extern "C" {

bool ensure_settings_config_dir(void)
{
	char *dir = obs_module_get_config_path(obs_current_module(), "");
	if (!dir) {
		obs_log(LOG_ERROR, "Failed to create plugin config directory");
		return false;
	}
	int res = os_mkdirs(dir);
	bfree(dir);
	if (res == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Failed to create plugin config directory");
		return false;
	}
	return true;
}

obs_data_t *overlay_settings_get(void)
{
	if (!g_saved_settings_data)
		g_saved_settings_data = obs_data_create();
	return g_saved_settings_data;
}

obs_data_t *overlay_settings_load(void)
{
	ensure_settings_config_dir();
	char *path = obs_module_get_config_path(obs_current_module(), "settings.json");
	if (g_saved_settings_data) {
		obs_data_release(g_saved_settings_data);
		g_saved_settings_data = nullptr;
	}
	if (path) {
		g_saved_settings_data = obs_data_create_from_json_file_safe(path, ".bak");
		if (g_saved_settings_data) {
			obs_log(LOG_INFO, "Settings loaded from %s", path);
		} else {
			obs_log(LOG_INFO, "No settings file at %s, using defaults", path);
			g_saved_settings_data = obs_data_create();
		}
		bfree(path);
	} else {
		g_saved_settings_data = obs_data_create();
	}
	return g_saved_settings_data;
}

bool overlay_settings_save(void)
{
	if (!g_saved_settings_data)
		return false;
	ensure_settings_config_dir();
	char *path = obs_module_get_config_path(obs_current_module(), "settings.json");
	if (!path)
		return false;
	bool ok = obs_data_save_json_safe(g_saved_settings_data, path, ".tmp", ".bak");
	if (ok)
		obs_log(LOG_INFO, "Settings saved to %s", path);
	else
		obs_log(LOG_WARNING, "Failed to save settings to %s", path);
	bfree(path);
	return ok;
}

void overlay_settings_free(void)
{
	if (g_saved_settings_data) {
		obs_data_release(g_saved_settings_data);
		g_saved_settings_data = nullptr;
	}
}

} // extern "C"
