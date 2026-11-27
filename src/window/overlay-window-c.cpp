#include "overlay-window-c.h"

#include "overlay-renderer.h"
#include "overlay-settings-storage.h"
#include "overlay-wayland-layer.h"
#include "plugin-support.h"

static OverlayRenderer *g_renderer = nullptr;

extern "C" {

bool overlay_window_create(void)
{
	if (g_renderer) {
		obs_log(LOG_INFO, "Overlay window already exists");
		return true;
	}
	g_renderer = new OverlayRenderer();
	if (!g_renderer->Initialize()) {
		delete g_renderer;
		g_renderer = nullptr;
		return false;
	}
	return true;
}

void overlay_window_destroy(void)
{
	if (g_renderer) {
		g_renderer->Shutdown();
		delete g_renderer;
		g_renderer = nullptr;
		obs_log(LOG_INFO, "overlay_window_destroy: Renderer destroyed");
	}
	overlay_win::WaylandLayerHelper::ShutdownGlobal();
}

void overlay_window_show(void)
{
	if (g_renderer)
		g_renderer->Show();
}

void overlay_window_hide(void)
{
	if (g_renderer)
		g_renderer->Hide();
}

void overlay_window_toggle(void)
{
	if (g_renderer)
		g_renderer->Toggle();
}

bool overlay_window_is_visible(void)
{
	return g_renderer && g_renderer->IsVisible();
}

void overlay_window_open_settings(void)
{
	if (g_renderer)
		g_renderer->OpenSettingsView();
}

void overlay_window_open_file_in_gallery(const char *file_path)
{
	if (g_renderer)
		g_renderer->OpenFileInGallery(file_path ? file_path : "");
}

void overlay_window_release_file_if_open(const char *file_path)
{
	if (g_renderer && file_path)
		g_renderer->ReleaseFileIfOpen(file_path);
}

void overlay_window_on_file_deleted(const char *file_path)
{
	if (g_renderer && file_path)
		g_renderer->OnExternalFileDeleted(file_path);
}

void overlay_window_notify_replay_saving(bool saving)
{
	if (g_renderer)
		g_renderer->NotifyReplaySaving(saving);
}

void overlay_window_reload_settings(void)
{
	if (g_renderer) {
		g_renderer->LoadSettings(overlay_settings_get());
		g_renderer->ApplyRuntimeSettings();
	}
}

} // extern "C"
