#include "overlay-indicators-c.h"

#include "overlay-indicators.h"
#include "plugin-support.h"

static OverlayIndicators *g_indicators = nullptr;

extern "C" {

bool overlay_indicators_create(void)
{
	if (!g_indicators) {
		g_indicators = new OverlayIndicators();
		if (!g_indicators->Initialize()) {
			delete g_indicators;
			g_indicators = nullptr;
			return false;
		}
	}
	return true;
}

void overlay_indicators_destroy(void)
{
	if (g_indicators) {
		g_indicators->Shutdown();
		delete g_indicators;
		g_indicators = nullptr;
		obs_log(LOG_INFO, "overlay_indicators_destroy: indicators destroyed");
	}
}

void overlay_indicators_set_enabled(bool enabled)
{
	if (g_indicators)
		g_indicators->SetEnabled(enabled);
}

void overlay_indicators_set_position(int pos)
{
	if (g_indicators)
		g_indicators->SetPosition(static_cast<OverlayPosition>(pos));
}

void overlay_indicators_set_oled_protection(bool enabled)
{
	if (g_indicators)
		g_indicators->SetOledProtection(enabled);
}

void overlay_indicators_set_scale(int scale_percent)
{
	if (g_indicators)
		g_indicators->SetScale(scale_percent);
}

void overlay_indicators_set_preview(bool preview)
{
	if (g_indicators)
		g_indicators->SetPreview(preview);
}

void overlay_indicators_set_monitor(int mode, int index)
{
	if (g_indicators)
		g_indicators->SetMonitor(static_cast<OverlayMonitorMode>(mode), index);
}

void overlay_indicators_set_custom_icon(int slot, const char *path, bool tint, uint32_t color_rgb)
{
	if (g_indicators)
		g_indicators->SetCustomIcon(slot, path ? std::string(path) : std::string{}, tint, color_rgb);
}

void overlay_indicators_set_sound_path(int event_idx, const char *path)
{
	if (g_indicators)
		g_indicators->SetSoundPath(event_idx, path ? std::string(path) : std::string{});
}

bool overlay_indicators_play_sound(int event_idx)
{
	if (!g_indicators)
		return false;
	return g_indicators->PlaySoundEvent(static_cast<OverlayIndicators::SoundEvent>(event_idx));
}

void overlay_indicators_update_state(bool recording, bool replay_active, bool replay_saving)
{
	if (g_indicators)
		g_indicators->UpdateState(recording, replay_active, replay_saving);
}

void overlay_indicators_notify_replay_saved(void)
{
	if (g_indicators)
		g_indicators->NotifyReplaySaved();
}

} // extern "C"
