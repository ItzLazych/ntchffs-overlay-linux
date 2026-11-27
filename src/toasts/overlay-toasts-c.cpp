#include "overlay-toasts-c.h"

#include "overlay-toasts.h"
#include "plugin-support.h"

static OverlayToasts *g_toasts = nullptr;

extern "C" {

bool overlay_toasts_create(void)
{
	if (!g_toasts) {
		g_toasts = new OverlayToasts();
		if (!g_toasts->Initialize()) {
			delete g_toasts;
			g_toasts = nullptr;
			return false;
		}
	}
	return true;
}

void overlay_toasts_destroy(void)
{
	if (g_toasts) {
		g_toasts->Shutdown();
		delete g_toasts;
		g_toasts = nullptr;
		obs_log(LOG_INFO, "overlay_toasts_destroy: toasts destroyed");
	}
}

void overlay_toasts_set_enabled(bool enabled)
{
	if (g_toasts)
		g_toasts->SetEnabled(enabled);
}

void overlay_toasts_set_video_saved(bool enabled)
{
	if (g_toasts)
		g_toasts->SetVideoSavedEnabled(enabled);
}

void overlay_toasts_set_export_enabled(bool enabled)
{
	if (g_toasts)
		g_toasts->SetExportEnabled(enabled);
}

void overlay_toasts_set_open_hint(bool enabled)
{
	if (g_toasts)
		g_toasts->SetOpenHintEnabled(enabled);
}

void overlay_toasts_set_low_disk(bool enabled, int threshold_gb)
{
	if (g_toasts) {
		g_toasts->SetLowDiskEnabled(enabled);
		g_toasts->SetLowDiskThresholdGb(threshold_gb);
	}
}

void overlay_toasts_set_duration_sec(int sec)
{
	if (g_toasts)
		g_toasts->SetDurationSec(sec);
}

void overlay_toasts_set_accent_color(uint32_t color_rgb)
{
	if (g_toasts)
		g_toasts->SetAccentColorRgb(color_rgb);
}

void overlay_toasts_set_hotkeys_labels(const char *view_label, const char *delete_label)
{
	if (g_toasts)
		g_toasts->SetHotkeysLabels(view_label ? view_label : "", delete_label ? delete_label : "");
}

void overlay_toasts_set_monitor(int mode, int index)
{
	if (g_toasts)
		g_toasts->SetMonitor(static_cast<OverlayMonitorMode>(mode), index);
}

void overlay_toasts_set_action_callback(overlay_toast_action_cb cb, void *userdata)
{
	if (!g_toasts)
		return;
	if (!cb) {
		g_toasts->SetActionHandler(nullptr);
		return;
	}
	g_toasts->SetActionHandler([cb, userdata](OverlayToasts::ActionKind kind, const std::string &path) {
		cb(static_cast<int>(kind), path.c_str(), userdata);
	});
}

void overlay_toasts_show_replay_saved(const char *file_path, bool is_recording)
{
	if (g_toasts && file_path)
		g_toasts->ShowReplaySavedToast(file_path, is_recording);
}

void overlay_toasts_show_export_result(bool success, bool merged, const char *detail_or_error)
{
	if (g_toasts)
		g_toasts->ShowExportToast(success, merged, detail_or_error ? detail_or_error : "");
}

void overlay_toasts_show_open_hint(const char *toggle_hotkey_label)
{
	if (g_toasts)
		g_toasts->ShowOpenHintToast(toggle_hotkey_label ? toggle_hotkey_label : "");
}

void overlay_toasts_check_low_disk(uint64_t free_bytes)
{
	if (g_toasts)
		g_toasts->CheckLowDiskSpace(free_bytes);
}

bool overlay_toasts_trigger_hotkey_action(int action_kind)
{
	if (!g_toasts)
		return false;
	return g_toasts->TriggerHotkeyAction(static_cast<OverlayToasts::ActionKind>(action_kind));
}

} // extern "C"
