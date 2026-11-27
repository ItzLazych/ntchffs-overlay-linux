#include "overlay-rebuild-preset.h"

#include <algorithm>
#include <cstdio>
#include <random>

OverlayExportOptions RebuildPreset::ToExportOptions() const
{
	OverlayExportOptions o;
	o.mode = mode;
	o.format = format;
	o.crf = crf;
	o.quality = quality;
	o.width = width;
	o.height = height;
	o.fps = fps;
	o.keepAudio = keepAudio;
	o.keepVideo = keepVideo;
	o.tracksConfigured = tracksConfigured;
	o.keepAudioOrdinals = keepAudioOrdinals;
	o.gifWidth = gifWidth;
	o.gifHeight = gifHeight;
	o.gifFps = gifFps;
	o.gifColors = gifColors;
	o.gifDither = gifDither;
	o.gifLoop = gifLoop;
	return o;
}

std::string RebuildPreset::ResolutionLabel() const
{
	if (width <= 0 && height <= 0)
		return "Source";
	if (width == 854 && height == 480)
		return "480p";
	if (width == 1280 && height == 720)
		return "720p";
	if (width == 1920 && height == 1080)
		return "1080p";
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%dx%d", width, height);
	return buf;
}

std::string RebuildPreset::FormatLabel() const
{
	switch (format) {
	case RebuildOutputFormat::Mp4:
		return "MP4";
	case RebuildOutputFormat::Mov:
		return "MOV";
	case RebuildOutputFormat::Gif:
		return "GIF";
	}
	return "MP4";
}

obs_data_t *SerializeRebuildPreset(const RebuildPreset &p)
{
	obs_data_t *obj = obs_data_create();
	obs_data_set_string(obj, "id", p.id.c_str());
	obs_data_set_string(obj, "name", p.name.c_str());
	obs_data_set_int(obj, "mode", static_cast<long long>(p.mode));
	obs_data_set_int(obj, "format", static_cast<long long>(p.format));
	obs_data_set_int(obj, "crf", p.crf);
	obs_data_set_int(obj, "quality", static_cast<long long>(p.quality));
	obs_data_set_int(obj, "width", p.width);
	obs_data_set_int(obj, "height", p.height);
	obs_data_set_double(obj, "fps", p.fps);
	obs_data_set_int(obj, "gif_width", p.gifWidth);
	obs_data_set_int(obj, "gif_height", p.gifHeight);
	obs_data_set_int(obj, "gif_fps", p.gifFps);
	obs_data_set_int(obj, "gif_colors", p.gifColors);
	obs_data_set_int(obj, "gif_dither", p.gifDither);
	obs_data_set_bool(obj, "gif_loop", p.gifLoop);
	obs_data_set_bool(obj, "keep_audio", p.keepAudio);
	obs_data_set_bool(obj, "keep_video", p.keepVideo);
	obs_data_set_bool(obj, "tracks_configured", p.tracksConfigured);

	obs_data_array_t *arr = obs_data_array_create();
	for (int ord : p.keepAudioOrdinals) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "i", ord);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(obj, "keep_audio_ordinals", arr);
	obs_data_array_release(arr);
	return obj;
}

RebuildPreset DeserializeRebuildPreset(obs_data_t *obj)
{
	RebuildPreset p;
	if (!obj)
		return p;
	if (const char *id = obs_data_get_string(obj, "id"))
		p.id = id;
	if (const char *name = obs_data_get_string(obj, "name"))
		p.name = name;
	p.mode = static_cast<RebuildMode>(std::clamp<int>(static_cast<int>(obs_data_get_int(obj, "mode")), 0, 1));
	p.format = static_cast<RebuildOutputFormat>(
		std::clamp<int>(static_cast<int>(obs_data_get_int(obj, "format")), 0, 2));
	if (obs_data_has_user_value(obj, "crf"))
		p.crf = static_cast<int>(obs_data_get_int(obj, "crf"));
	p.quality = static_cast<RebuildQuality>(
		std::clamp<int>(static_cast<int>(obs_data_get_int(obj, "quality")), 0, 3));
	p.width = static_cast<int>(obs_data_get_int(obj, "width"));
	p.height = static_cast<int>(obs_data_get_int(obj, "height"));
	p.fps = obs_data_get_double(obj, "fps");
	if (obs_data_has_user_value(obj, "gif_width"))
		p.gifWidth = static_cast<int>(obs_data_get_int(obj, "gif_width"));
	p.gifHeight = static_cast<int>(obs_data_get_int(obj, "gif_height"));
	if (obs_data_has_user_value(obj, "gif_fps"))
		p.gifFps = static_cast<int>(obs_data_get_int(obj, "gif_fps"));
	if (obs_data_has_user_value(obj, "gif_colors"))
		p.gifColors = static_cast<int>(obs_data_get_int(obj, "gif_colors"));
	if (obs_data_has_user_value(obj, "gif_dither"))
		p.gifDither = static_cast<int>(obs_data_get_int(obj, "gif_dither"));
	if (obs_data_has_user_value(obj, "gif_loop"))
		p.gifLoop = obs_data_get_bool(obj, "gif_loop");
	if (obs_data_has_user_value(obj, "keep_audio"))
		p.keepAudio = obs_data_get_bool(obj, "keep_audio");
	if (obs_data_has_user_value(obj, "keep_video"))
		p.keepVideo = obs_data_get_bool(obj, "keep_video");
	p.tracksConfigured = obs_data_get_bool(obj, "tracks_configured");

	if (obs_data_array_t *arr = obs_data_get_array(obj, "keep_audio_ordinals")) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; ++i) {
			if (obs_data_t *item = obs_data_array_item(arr, i)) {
				p.keepAudioOrdinals.push_back(static_cast<int>(obs_data_get_int(item, "i")));
				obs_data_release(item);
			}
		}
		obs_data_array_release(arr);
	}
	return p;
}

void RebuildPresetStore::SeedDefaultsIfNeeded(obs_data_t *settings)
{
	bool seeded = settings && obs_data_get_bool(settings, "rebuild_presets_seeded");
	if (seeded && !presets_.empty())
		return;

	presets_.clear();

	RebuildPreset balanced;
	balanced.id = "seed_balanced";
	balanced.name = "Balanced";
	balanced.mode = RebuildMode::Reencode;
	balanced.format = RebuildOutputFormat::Mp4;
	balanced.crf = 23;
	balanced.quality = RebuildQuality::Medium;

	if (settings && obs_data_get_bool(settings, "gallery_share_compress")) {
		int q = static_cast<int>(obs_data_get_int(settings, "gallery_share_compress_quality"));
		balanced.crf = std::clamp(q > 0 ? q : 26, 16, 40);
	}

	RebuildPreset copyPreset;
	copyPreset.id = "seed_copy";
	copyPreset.name = "Copy (Lossless Trim)";
	copyPreset.mode = RebuildMode::Copy;
	copyPreset.format = RebuildOutputFormat::Mp4;

	RebuildPreset quality;
	quality.id = "seed_quality";
	quality.name = "Quality";
	quality.mode = RebuildMode::Reencode;
	quality.format = RebuildOutputFormat::Mp4;
	quality.crf = 18;
	quality.quality = RebuildQuality::High;

	RebuildPreset strong;
	strong.id = "seed_strong";
	strong.name = "Strong Compression";
	strong.mode = RebuildMode::Reencode;
	strong.format = RebuildOutputFormat::Mp4;
	strong.crf = 30;
	strong.quality = RebuildQuality::Low;
	strong.width = 1280;
	strong.height = 720;

	presets_.push_back(std::move(copyPreset));
	presets_.push_back(std::move(balanced));
	presets_.push_back(std::move(quality));
	presets_.push_back(std::move(strong));
	selectedId_ = "seed_balanced";

	if (settings) {
		obs_data_set_bool(settings, "rebuild_presets_seeded", true);
		SaveToSettings(settings);
	}
}

void RebuildPresetStore::LoadFromSettings(obs_data_t *settings)
{
	presets_.clear();
	if (settings) {
		if (obs_data_array_t *arr = obs_data_get_array(settings, "rebuild_presets")) {
			size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; ++i) {
				if (obs_data_t *item = obs_data_array_item(arr, i)) {
					RebuildPreset p = DeserializeRebuildPreset(item);
					if (!p.id.empty())
						presets_.push_back(std::move(p));
					obs_data_release(item);
				}
			}
			obs_data_array_release(arr);
		}
		if (const char *sel = obs_data_get_string(settings, "rebuild_preset_selected")) {
			if (*sel)
				selectedId_ = sel;
		}
	}

	SeedDefaultsIfNeeded(settings);
	if (!FindById(selectedId_) && !presets_.empty())
		selectedId_ = presets_.front().id;
}

void RebuildPresetStore::SaveToSettings(obs_data_t *settings) const
{
	if (!settings)
		return;
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &p : presets_) {
		obs_data_t *item = SerializeRebuildPreset(p);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "rebuild_presets", arr);
	obs_data_array_release(arr);
	obs_data_set_string(settings, "rebuild_preset_selected", selectedId_.c_str());
	obs_data_set_bool(settings, "rebuild_presets_seeded", true);
}

void RebuildPresetStore::SetSelectedId(const std::string &id)
{
	if (FindById(id))
		selectedId_ = id;
}

const RebuildPreset *RebuildPresetStore::FindById(const std::string &id) const
{
	for (const auto &p : presets_) {
		if (p.id == id)
			return &p;
	}
	return nullptr;
}

const RebuildPreset *RebuildPresetStore::SelectedPreset() const
{
	if (const auto *p = FindById(selectedId_))
		return p;
	return presets_.empty() ? nullptr : &presets_.front();
}

RebuildPreset RebuildPresetStore::CreateCustomPreset(const std::string &baseName, const RebuildPreset &tmpl)
{
	std::random_device rd;
	uint32_t rnd = rd() & 0xFFFFFFu;
	char idBuf[32];
	std::snprintf(idBuf, sizeof(idBuf), "preset_%06x", rnd);

	RebuildPreset p = tmpl;
	p.id = idBuf;
	p.name = baseName.empty() ? "Custom Preset" : baseName;
	presets_.push_back(p);
	selectedId_ = p.id;
	return p;
}

bool RebuildPresetStore::UpdatePreset(const RebuildPreset &preset)
{
	for (auto &p : presets_) {
		if (p.id == preset.id) {
			p = preset;
			return true;
		}
	}
	return false;
}

bool RebuildPresetStore::DeletePreset(const std::string &id)
{
	auto it = std::remove_if(presets_.begin(), presets_.end(),
				 [&](const RebuildPreset &p) { return p.id == id; });
	if (it == presets_.end())
		return false;
	presets_.erase(it, presets_.end());
	if (presets_.empty()) {
		SeedDefaultsIfNeeded(nullptr);
	} else if (selectedId_ == id) {
		selectedId_ = presets_.front().id;
	}
	return true;
}
