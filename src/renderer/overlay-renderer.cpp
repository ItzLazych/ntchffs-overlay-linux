#include "overlay-renderer.h"

#include <QColor>
#include <QColorDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QInputDialog>
#include <QMainWindow>
#include <QMetaObject>
#include <QPainter>
#include <QUrl>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "overlay-clip-save.h"
#include "overlay-indicators-c.h"
#include "overlay-settings-storage.h"
#include "overlay-smart-replay.h"
#include "overlay-string-utils.h"
#include "overlay-svg-rasterize.h"
#include "overlay-toasts-c.h"
#include "plugin-support.h"

namespace {

inline uint32_t RgbFromSavedValue(int64_t val)
{
	// Windows saved COLORREF (0x00BBGGRR). Detect/store canonical RGB or convert if needed.
	// Note: 0x00A882FF in BGR was #FF82A8 in RGB. For consistency on Linux we store 0xRRGGBB with high bit tag or clean BGR->RGB helper.
	uint32_t u = static_cast<uint32_t>(val) & 0xFFFFFFu;
	return u;
}

float MeasureButtonWidth(const std::string &utf8, float fontSize, float minW, float padX = 22.0f)
{
	if (utf8.empty())
		return minW;
	overlay_compat::TextFormat fmt(QStringLiteral("Inter, Sans Serif"), fontSize, QFont::Medium);
	QString qs = overlay_str::Utf8ToQString(utf8);
	float maxLineW = 0.0f;
	for (const QString &ln : qs.split(QLatin1Char('\n'))) {
		auto sz = fmt.Measure(ln);
		maxLineW = std::max(maxLineW, sz.width);
	}
	return std::max(minW, std::ceil(maxLineW + padX));
}

} // namespace

OverlayRenderer::OverlayRenderer(QObject *parent) : QObject(parent)
{
	pollTimer_.setInterval(500);
	connect(&pollTimer_, &QTimer::timeout, this, [this]() { RefreshState(); });

	animTimer_.setInterval(33);
	connect(&animTimer_, &QTimer::timeout, this, [this]() {
		if (!IsVisible())
			return;
		if (cfg_.autoHideEnabled && viewMode_ == ViewMode::HudOnly && lastInteractionMs_ > 0) {
			uint64_t now = overlay_compat::GetTickCount64Ms();
			if (now - lastInteractionMs_ >=
			    static_cast<uint64_t>(std::max(2, cfg_.autoHideSeconds)) * 1000ULL) {
				Hide();
				return;
			}
		}
		if (viewMode_ == ViewMode::Gallery || state_.replaySaving)
			winMgr_.RequestRepaint();
	});
}

OverlayRenderer::~OverlayRenderer()
{
	Shutdown();
}

QImage OverlayRenderer::LoadAssetImage(const std::string &relPath, int targetPx)
{
	char *p = obs_module_file(relPath.c_str());
	if (!p)
		return {};
	std::string pathStr(p);
	bfree(p);

	QImage out;
	if (overlay_str::EndsWithCaseInsensitive(pathStr, ".svg")) {
		overlay::svg::RasterizeSvgFile(pathStr, targetPx > 0 ? targetPx : 48,
					       targetPx > 0 ? targetPx : 48, out);
	} else {
		out.load(overlay_str::Utf8ToQString(pathStr));
		if (!out.isNull())
			out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	return out;
}

void OverlayRenderer::LoadBundledAssets()
{
	icoArrowLeft_ = LoadAssetImage("icons/arrow-left.png");
	icoArrowsIn_ = LoadAssetImage("icons/arrows-in.png");
	icoArrowsOut_ = LoadAssetImage("icons/arrows-out.png");
	icoStepBack_ = LoadAssetImage("icons/caret-line-left.png");
	icoStepFwd_ = LoadAssetImage("icons/caret-line-right.png");
	icoChart_ = LoadAssetImage("icons/chart-line.png");
	icoDownload_ = LoadAssetImage("icons/download-simple.png");
	icoGear_ = LoadAssetImage("icons/gear.png");
	icoPause_ = LoadAssetImage("icons/pause.png");
	icoPlay_ = LoadAssetImage("icons/play.png");
	icoPlus_ = LoadAssetImage("icons/plus.png");
	icoSpeaker_ = LoadAssetImage("icons/speaker-high.png");
	icoSpeakerMute_ = LoadAssetImage("icons/speaker-slash.png");
	icoTrash_ = LoadAssetImage("icons/trash-simple.png");

	logoPng_ = LoadAssetImage("logo/logo.png");
	svgGithub_ = LoadAssetImage("logo/github.svg", 32);
	svgTelegram_ = LoadAssetImage("logo/telegram.svg", 32);
	svgTwitch_ = LoadAssetImage("logo/twitch.svg", 32);
	svgDonation_ = LoadAssetImage("logo/donationalerts.svg", 32);
}

std::string OverlayRenderer::ResolveConfigFile(const std::string &filename) const
{
	char *p = obs_module_get_config_path(obs_current_module(), filename.c_str());
	if (!p)
		return {};
	std::string res(p);
	bfree(p);
	return res;
}

bool OverlayRenderer::Initialize()
{
	if (!winMgr_.Initialize())
		return false;

	LoadBundledAssets();

	winMgr_.SetPaintCallback([this](QImage &canvas, const QRect &dirty) { RenderFrame(canvas, dirty); });
	winMgr_.SetMouseMoveCallback([this](float x, float y) { HandleMouseMove(x, y); });
	winMgr_.SetMouseButtonCallback(
		[this](float x, float y, Qt::MouseButton btn, bool pressed) { HandleMouseButton(x, y, btn, pressed); });
	winMgr_.SetWheelCallback([this](float x, float y, int dy) { HandleWheel(x, y, dy); });
	winMgr_.SetKeyCallback(
		[this](int k, uint32_t m, const QString &txt, bool pr) { HandleKey(k, m, txt, pr); });
	winMgr_.SetVisibilityCallback([this](bool vis) {
		if (vis) {
			TouchInteractionClock();
			RefreshState();
			if (viewMode_ == ViewMode::Gallery)
				RefreshGalleryList();
			if (!pollTimer_.isActive())
				pollTimer_.start();
			if (!animTimer_.isActive())
				animTimer_.start();
		} else {
			animTimer_.stop();
			if (cfg_.returnHomeOnHide) {
				player_.Close();
				viewMode_ = ViewMode::HudOnly;
				cfg_.indicatorsPreview = false;
				overlay_indicators_set_preview(false);
			}
			winMgr_.SetKeyboardInteractive(false);
		}
	});

	exportQueue_.SetConfigPath(ResolveConfigFile("export_queue.json"));
	exportQueue_.SetCallbacks(
		[this]() {
			QMetaObject::invokeMethod(
				this,
				[this]() {
					if (IsVisible())
						winMgr_.RequestRepaint();
				},
				Qt::QueuedConnection);
		},
		[this](const ExportQueueJob &job, bool merged, const std::string &mergedPath) {
			bool ok = (job.status == ExportJobStatus::Done);
			std::string detail = ok ? (merged ? mergedPath : job.outputPath) : job.errorText;
			QMetaObject::invokeMethod(
				this,
				[this, ok, merged, detail]() {
					overlay_toasts_show_export_result(ok, merged, detail.c_str());
					RefreshGalleryList();
					if (IsVisible())
						winMgr_.RequestRepaint();
				},
				Qt::QueuedConnection);
		});
	exportQueue_.LoadFromDisk();

	LoadSettings(overlay_settings_get());
	ApplyRuntimeSettings();
	pollTimer_.start();

	obs_log(LOG_INFO, "OverlayRenderer initialized successfully");
	return true;
}

void OverlayRenderer::Shutdown()
{
	pollTimer_.stop();
	animTimer_.stop();
	player_.Close();
	gallery_.StopWorkers();
	winMgr_.Shutdown();
}

void OverlayRenderer::Show()
{
	winMgr_.SetMonitorSelection(cfg_.monitorMode, cfg_.monitorIndex);
	winMgr_.SetKeyboardInteractive(viewMode_ != ViewMode::HudOnly);
	RefreshState();
	TouchInteractionClock();
	winMgr_.Show();
	winMgr_.RequestRepaint();
}

void OverlayRenderer::Hide()
{
	if (cfg_.returnHomeOnHide && viewMode_ != ViewMode::HudOnly)
		viewMode_ = ViewMode::HudOnly;
	cfg_.indicatorsPreview = false;
	overlay_indicators_set_preview(false);
	winMgr_.SetKeyboardInteractive(false);
	winMgr_.Hide();
}

void OverlayRenderer::Toggle()
{
	if (IsVisible())
		Hide();
	else
		Show();
}

bool OverlayRenderer::IsVisible() const
{
	return winMgr_.IsVisible();
}

void OverlayRenderer::OpenSettingsView()
{
	viewMode_ = ViewMode::Settings;
	settingsTab_ = SettingsTab::General;
	winMgr_.SetKeyboardInteractive(true);
	Show();
	winMgr_.RequestRepaint();
}

void OverlayRenderer::OpenFileInGallery(const std::string &filePath)
{
	viewMode_ = ViewMode::Gallery;
	winMgr_.SetKeyboardInteractive(true);
	Show();
	RefreshGalleryList();
	if (!filePath.empty())
		SelectMediaItem(filePath);
	winMgr_.RequestRepaint();
}

void OverlayRenderer::ReleaseFileIfOpen(const std::string &filePath)
{
	if (player_.IsOpen() && player_.FilePath() == filePath) {
		obs_log(LOG_INFO, "Gallery: releasing open player file before external delete");
		player_.Close();
	}
	if (selectedMediaPath_ == filePath) {
		obs_log(LOG_INFO, "Gallery: releasing selected player file before external delete");
	}
}

void OverlayRenderer::OnExternalFileDeleted(const std::string &filePath)
{
	ReleaseFileIfOpen(filePath);
	if (selectedMediaPath_ == filePath)
		selectedMediaPath_.clear();
	RefreshGalleryList();
	if (IsVisible())
		winMgr_.RequestRepaint();
}

void OverlayRenderer::NotifyReplaySaving(bool saving)
{
	replaySavingFlag_ = saving;
	state_.replaySaving = saving;
	overlay_indicators_update_state(state_.recordingActive, state_.replayActive, saving);
	if (IsVisible())
		winMgr_.RequestRepaint();
}

void OverlayRenderer::LoadSettings(obs_data_t *s)
{
	if (!s)
		return;

	auto getBoolDef = [&](const char *k, bool def) {
		return obs_data_has_user_value(s, k) ? obs_data_get_bool(s, k) : def;
	};
	auto getIntDef = [&](const char *k, int def) {
		return obs_data_has_user_value(s, k) ? static_cast<int>(obs_data_get_int(s, k)) : def;
	};

	cfg_.position = static_cast<OverlayPosition>(std::clamp(getIntDef("position", 4), 0, 8));
	cfg_.margin = std::clamp(getIntDef("margin", 20), 0, 200);
	cfg_.orientation = static_cast<OverlayOrientation>(std::clamp(getIntDef("orientation", 0), 0, 1));
	cfg_.monitorMode = static_cast<OverlayMonitorMode>(
		std::clamp(getIntDef("overlay_monitor_mode", 0), 0, 2));
	cfg_.monitorIndex = std::max(0, getIntDef("overlay_monitor_index", 0));
	cfg_.statsVisible = getBoolDef("stats_visible", true);
	cfg_.autoHideEnabled = getBoolDef("auto_hide_enabled", false);
	cfg_.autoHideSeconds = std::clamp(getIntDef("auto_hide_seconds", 10), 2, 300);
	cfg_.galleryInOverlay = getBoolDef("gallery_in_overlay", true);
	cfg_.returnHomeOnHide = getBoolDef("return_home_on_hide", true);
	cfg_.captureFocus = getBoolDef("capture_focus", false);
	cfg_.backgroundAlphaPercent = std::clamp(getIntDef("overlay_background_alpha", 82), 20, 100);
	cfg_.scalePercent = std::clamp(getIntDef("overlay_scale", 100), 60, 200);
	cfg_.textScalePercent = std::clamp(getIntDef("overlay_text_scale", 100), 70, 160);
	if (obs_data_has_user_value(s, "overlay_accent_color"))
		cfg_.accentRgb = RgbFromSavedValue(obs_data_get_int(s, "overlay_accent_color"));

	cfg_.galleryMuted = getBoolDef("gallery_muted", false);
	cfg_.galleryVolume = obs_data_has_user_value(s, "gallery_volume")
				     ? static_cast<float>(std::clamp(obs_data_get_double(s, "gallery_volume"), 0.0, 1.0))
				     : 0.8f;
	cfg_.galleryFullscreen = getBoolDef("gallery_fullscreen", false);
	if (const char *ep = obs_data_get_string(s, "gallery_export_path"))
		cfg_.galleryExportPath = ep;
	cfg_.exportMaxParallel = std::clamp(getIntDef("gallery_export_max_parallel", 2), 1, 4);
	cfg_.shareCompress = getBoolDef("gallery_share_compress", false);
	cfg_.shareCompressCrf = std::clamp(getIntDef("gallery_share_crf", 26), 14, 42);

	const int defSecs[4] = {15, 30, 60, 120};
	for (int i = 0; i < 4; ++i) {
		std::string ks = "clip_slot_" + std::to_string(i) + "_seconds";
		std::string ksh = "clip_slot_" + std::to_string(i) + "_show";
		cfg_.clipSlotSeconds[i] = std::clamp(getIntDef(ks.c_str(), defSecs[i]), 1, 3600);
		cfg_.clipSlotShow[i] = getBoolDef(ksh.c_str(), i < 3);
	}

	cfg_.indicatorsEnabled = getBoolDef("indicators_enabled", true);
	cfg_.indicatorsPosition = static_cast<OverlayPosition>(
		std::clamp(getIntDef("indicators_position", static_cast<int>(OverlayPosition::TopRight)), 0, 8));
	cfg_.indicatorsOled = getBoolDef("indicators_oled_protection", false);
	cfg_.indicatorsScale = std::clamp(getIntDef("indicators_scale", 100), 50, 250);

	static constexpr const char *kSlots[4] = {"recording", "replay", "saving", "saved"};
	static constexpr uint32_t kDefCols[4] = {0xEF4444, 0x22C55E, 0xF59E0B, 0x38BDF8};
	for (int i = 0; i < 4; ++i) {
		std::string base = std::string("indicators_") + kSlots[i];
		if (const char *ip = obs_data_get_string(s, (base + "_icon").c_str()))
			cfg_.indicatorIcons[i] = ip;
		cfg_.indicatorTints[i] = getBoolDef((base + "_tint").c_str(), false);
		cfg_.indicatorColors[i] =
			obs_data_has_user_value(s, (base + "_color").c_str())
				? RgbFromSavedValue(obs_data_get_int(s, (base + "_color").c_str()))
				: kDefCols[i];
	}
	static constexpr const char *kSounds[5] = {"recording_on", "recording_off", "replay_on", "replay_off",
						   "replay_saved"};
	for (int i = 0; i < 5; ++i) {
		std::string sk = std::string("indicators_sound_") + kSounds[i];
		if (const char *sp = obs_data_get_string(s, sk.c_str()))
			cfg_.indicatorSounds[i] = sp;
	}

	cfg_.rememberReplayBuffer = getBoolDef("remember_replay_buffer", false);
	cfg_.smartReplayEnabled = getBoolDef("smart_replay", false);
	cfg_.smartReplayMode = std::clamp(getIntDef("smart_replay_mode", 0), 0, 1);
	cfg_.smartReplayClipIgnoreTrim = getBoolDef("smart_replay_clip_ignore_trim", false);

	cfg_.toastsEnabled = getBoolDef("toasts_enabled", true);
	cfg_.toastVideoSaved = getBoolDef("toast_video_saved", true);
	cfg_.toastExport = getBoolDef("toast_export", true);
	cfg_.toastOpenHint = getBoolDef("toast_open_hint", true);
	cfg_.toastLowDisk = getBoolDef("toast_low_disk", true);
	cfg_.toastLowDiskGb = std::clamp(getIntDef("toast_low_disk_gb", 5), 1, 500);
	cfg_.toastDurationSec = std::clamp(getIntDef("toast_duration_sec", 8), 1, 60);

	presetStore_.LoadFromSettings(s);
	SyncDraftFromSelectedPreset();
}

void OverlayRenderer::SyncDraftFromSelectedPreset()
{
	if (const RebuildPreset *p = presetStore_.SelectedPreset()) {
		draftPreset_ = *p;
	} else {
		draftPreset_ = RebuildPreset{};
		draftPreset_.id = "seed_balanced";
		draftPreset_.name = "Balanced";
	}
	keepVideo_ = draftPreset_.keepVideo;
	draftPresetInitialized_ = true;
}

void OverlayRenderer::SaveSettings(obs_data_t *s)
{
	if (!s)
		return;

	obs_data_set_int(s, "position", static_cast<long long>(cfg_.position));
	obs_data_set_int(s, "margin", cfg_.margin);
	obs_data_set_int(s, "orientation", static_cast<long long>(cfg_.orientation));
	obs_data_set_int(s, "overlay_monitor_mode", static_cast<long long>(cfg_.monitorMode));
	obs_data_set_int(s, "overlay_monitor_index", cfg_.monitorIndex);
	obs_data_set_bool(s, "stats_visible", cfg_.statsVisible);
	obs_data_set_bool(s, "auto_hide_enabled", cfg_.autoHideEnabled);
	obs_data_set_int(s, "auto_hide_seconds", cfg_.autoHideSeconds);
	obs_data_set_bool(s, "gallery_in_overlay", cfg_.galleryInOverlay);
	obs_data_set_bool(s, "return_home_on_hide", cfg_.returnHomeOnHide);
	obs_data_set_bool(s, "capture_focus", cfg_.captureFocus);
	obs_data_set_int(s, "overlay_background_alpha", cfg_.backgroundAlphaPercent);
	obs_data_set_int(s, "overlay_scale", cfg_.scalePercent);
	obs_data_set_int(s, "overlay_text_scale", cfg_.textScalePercent);
	obs_data_set_int(s, "overlay_accent_color", cfg_.accentRgb);

	obs_data_set_bool(s, "gallery_muted", cfg_.galleryMuted);
	obs_data_set_double(s, "gallery_volume", cfg_.galleryVolume);
	obs_data_set_bool(s, "gallery_fullscreen", cfg_.galleryFullscreen);
	obs_data_set_string(s, "gallery_export_path", cfg_.galleryExportPath.c_str());
	obs_data_set_int(s, "gallery_export_max_parallel", cfg_.exportMaxParallel);
	obs_data_set_bool(s, "gallery_share_compress", cfg_.shareCompress);
	obs_data_set_int(s, "gallery_share_crf", cfg_.shareCompressCrf);

	for (int i = 0; i < 4; ++i) {
		obs_data_set_int(s, ("clip_slot_" + std::to_string(i) + "_seconds").c_str(),
				 cfg_.clipSlotSeconds[i]);
		obs_data_set_bool(s, ("clip_slot_" + std::to_string(i) + "_show").c_str(),
				  cfg_.clipSlotShow[i]);
	}

	obs_data_set_bool(s, "indicators_enabled", cfg_.indicatorsEnabled);
	obs_data_set_int(s, "indicators_position", static_cast<long long>(cfg_.indicatorsPosition));
	obs_data_set_bool(s, "indicators_oled_protection", cfg_.indicatorsOled);
	obs_data_set_int(s, "indicators_scale", cfg_.indicatorsScale);

	static constexpr const char *kSlots[4] = {"recording", "replay", "saving", "saved"};
	for (int i = 0; i < 4; ++i) {
		std::string base = std::string("indicators_") + kSlots[i];
		obs_data_set_string(s, (base + "_icon").c_str(), cfg_.indicatorIcons[i].c_str());
		obs_data_set_bool(s, (base + "_tint").c_str(), cfg_.indicatorTints[i]);
		obs_data_set_int(s, (base + "_color").c_str(), cfg_.indicatorColors[i]);
	}
	static constexpr const char *kSounds[5] = {"recording_on", "recording_off", "replay_on", "replay_off",
						   "replay_saved"};
	for (int i = 0; i < 5; ++i) {
		std::string sk = std::string("indicators_sound_") + kSounds[i];
		obs_data_set_string(s, sk.c_str(), cfg_.indicatorSounds[i].c_str());
	}

	obs_data_set_bool(s, "remember_replay_buffer", cfg_.rememberReplayBuffer);
	obs_data_set_bool(s, "smart_replay", cfg_.smartReplayEnabled);
	obs_data_set_int(s, "smart_replay_mode", cfg_.smartReplayMode);
	obs_data_set_bool(s, "smart_replay_clip_ignore_trim", cfg_.smartReplayClipIgnoreTrim);

	obs_data_set_bool(s, "toasts_enabled", cfg_.toastsEnabled);
	obs_data_set_bool(s, "toast_video_saved", cfg_.toastVideoSaved);
	obs_data_set_bool(s, "toast_export", cfg_.toastExport);
	obs_data_set_bool(s, "toast_open_hint", cfg_.toastOpenHint);
	obs_data_set_bool(s, "toast_low_disk", cfg_.toastLowDisk);
	obs_data_set_int(s, "toast_low_disk_gb", cfg_.toastLowDiskGb);
	obs_data_set_int(s, "toast_duration_sec", cfg_.toastDurationSec);

	presetStore_.SaveToSettings(s);
}

void OverlayRenderer::ApplyRuntimeSettings()
{
	winMgr_.SetMonitorSelection(cfg_.monitorMode, cfg_.monitorIndex);
	winMgr_.SetCaptureFocus(cfg_.captureFocus);
	exportQueue_.SetMaxParallel(cfg_.exportMaxParallel);
	player_.SetMuted(cfg_.galleryMuted);
	player_.SetVolume(cfg_.galleryVolume);

	overlay_indicators_set_monitor(static_cast<int>(cfg_.monitorMode), cfg_.monitorIndex);
	overlay_indicators_set_enabled(cfg_.indicatorsEnabled);
	overlay_indicators_set_position(static_cast<int>(cfg_.indicatorsPosition));
	overlay_indicators_set_oled_protection(cfg_.indicatorsOled);
	overlay_indicators_set_scale(cfg_.indicatorsScale);
	overlay_indicators_set_preview(cfg_.indicatorsPreview);
	for (int i = 0; i < 4; ++i) {
		overlay_indicators_set_custom_icon(i, cfg_.indicatorIcons[i].c_str(), cfg_.indicatorTints[i],
						   cfg_.indicatorColors[i]);
	}
	for (int i = 0; i < 5; ++i)
		overlay_indicators_set_sound_path(i, cfg_.indicatorSounds[i].c_str());

	overlay_toasts_set_monitor(static_cast<int>(cfg_.monitorMode), cfg_.monitorIndex);
	overlay_toasts_set_enabled(cfg_.toastsEnabled);
	overlay_toasts_set_video_saved(cfg_.toastVideoSaved);
	overlay_toasts_set_export_enabled(cfg_.toastExport);
	overlay_toasts_set_open_hint(cfg_.toastOpenHint);
	overlay_toasts_set_low_disk(cfg_.toastLowDisk, cfg_.toastLowDiskGb);
	overlay_toasts_set_duration_sec(cfg_.toastDurationSec);
	overlay_toasts_set_accent_color(cfg_.accentRgb);

	overlay_smart_replay_set_enabled(cfg_.smartReplayEnabled);
	overlay_smart_replay_set_mode(cfg_.smartReplayMode);
	overlay_smart_replay_set_clip_ignore_trim(cfg_.smartReplayClipIgnoreTrim);

	if (IsVisible())
		winMgr_.RequestRepaint();
}

void OverlayRenderer::SaveAndApplySettings()
{
	SaveSettings(overlay_settings_get());
	overlay_settings_save();
	ApplyRuntimeSettings();
}

void OverlayRenderer::RefreshState()
{
	OverlayState fresh = OverlayStateManager::Refresh();
	fresh.statsVisible = cfg_.statsVisible;
	fresh.replaySaving = replaySavingFlag_;

	// Fire indicators sounds / transitions if state toggled
	if (fresh.recordingActive != prevRecordingState_) {
		prevRecordingState_ = fresh.recordingActive;
		overlay_indicators_play_sound(fresh.recordingActive ? 0 : 1);
	}
	if (fresh.replayActive != prevReplayState_) {
		prevReplayState_ = fresh.replayActive;
		overlay_indicators_play_sound(fresh.replayActive ? 2 : 3);
	}

	state_ = fresh;
	overlay_indicators_update_state(state_.recordingActive, state_.replayActive, state_.replaySaving);
	if (state_.freeSpaceBytes > 0)
		overlay_toasts_check_low_disk(state_.freeSpaceBytes);

	if (IsVisible())
		winMgr_.RequestRepaint();
}

void OverlayRenderer::RefreshGalleryList()
{
	std::string dir = OverlayStateManager::ResolveObsRecordingDirectory();
	gallery_.Refresh(dir, [this]() {
		QMetaObject::invokeMethod(
			this,
			[this]() {
				if (IsVisible() && viewMode_ == ViewMode::Gallery)
					winMgr_.RequestRepaint();
			},
			Qt::QueuedConnection);
	});
	if (selectedMediaPath_.empty()) {
		auto items = gallery_.SnapshotItems();
		if (!items.empty())
			SelectMediaItem(items.front().filePath);
	}
}

void OverlayRenderer::SelectMediaItem(const std::string &filePath)
{
	if (filePath.empty())
		return;
	selectedMediaPath_ = filePath;
	editingJobId_.clear();
	player_.Open(filePath, [this]() {
		QMetaObject::invokeMethod(
			this,
			[this]() {
				if (IsVisible() && viewMode_ == ViewMode::Gallery)
					winMgr_.RequestRepaint();
			},
			Qt::QueuedConnection);
	});
	player_.SetMuted(cfg_.galleryMuted);
	player_.SetVolume(cfg_.galleryVolume);
	trimStartSec_ = 0.0;
	trimEndSec_ = player_.DurationSeconds();
	selectedAudioStreams_.clear();
	for (const auto &as : player_.AudioStreams())
		selectedAudioStreams_.push_back(as.streamIndex);
}

void OverlayRenderer::AddHitZone(const std::string &id, const overlay_compat::RectF &r,
				 std::function<void(float, float, Qt::MouseButton)> onPress,
				 std::function<void(float, float)> onDrag,
				 std::function<void()> onRelease,
				 std::function<void(int)> onWheel)
{
	HitZone z;
	z.id = id;
	z.rect = r;
	z.onPress = std::move(onPress);
	z.onDrag = std::move(onDrag);
	z.onRelease = std::move(onRelease);
	z.onWheel = std::move(onWheel);
	hitZones_.push_back(std::move(z));
}

void OverlayRenderer::DrawButtonTile(overlay_compat::Canvas &c, const overlay_compat::RectF &r,
				     const std::string &id, const std::string &label, const QImage *icon,
				     bool active, overlay_compat::ColorF activeCol, float fontSize,
				     bool badgeDot)
{
	const bool hov = IsHovered(id);
	overlay_compat::ColorF bg = active ? overlay_compat::ColorF(activeCol.r, activeCol.g, activeCol.b,
								    hov ? 0.34f : 0.24f)
					   : overlay_compat::ColorF(0x232736, hov ? 0.92f : 0.72f);
	overlay_compat::ColorF border = active ? overlay_compat::ColorF(activeCol.r, activeCol.g, activeCol.b, 0.85f)
					       : overlay_compat::ColorF(0xFFFFFF, hov ? 0.28f : 0.10f);

	c.FillRoundedRect(overlay_compat::MakeRoundedRect(r, 8.0f, 8.0f), bg);
	c.DrawRoundedRect(overlay_compat::MakeRoundedRect(r, 8.0f, 8.0f), border, 1.0f);

	if (icon && !icon->isNull() && label.empty()) {
		float icoSz = std::min(r.Width(), r.Height()) * 0.56f;
		auto ir = overlay_compat::MakeRectXYWH(r.left + (r.Width() - icoSz) * 0.5f,
						       r.top + (r.Height() - icoSz) * 0.5f, icoSz, icoSz);
		c.DrawImage(*icon, ir);
	} else {
		float padX = std::clamp(r.Width() * 0.06f, 4.0f, 8.0f);
		float padY = std::clamp(r.Height() * 0.08f, 2.0f, 5.0f);
		auto textR = overlay_compat::MakeRectXYWH(r.left + padX, r.top + padY,
							  std::max(1.0f, r.Width() - padX * 2.0f),
							  std::max(1.0f, r.Height() - padY * 2.0f));
		QString qs = overlay_str::Utf8ToQString(label);
		float fitFs = fontSize;
		const float minFs = std::max(7.5f, fontSize * 0.62f);
		QStringList words;
		for (const QString &ln : qs.split(QLatin1Char('\n'))) {
			for (const QString &w : ln.split(QLatin1Char(' '), Qt::SkipEmptyParts))
				words.push_back(w);
		}
		while (fitFs > minFs + 0.1f) {
			overlay_compat::TextFormat probe(QStringLiteral("Inter, Sans Serif"), fitFs, QFont::Medium);
			probe.SetTextAlignment(overlay_compat::TextAlign::Center);
			probe.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
			probe.SetWordWrapping(overlay_compat::WordWrap::Wrap);
			bool wordsFit = true;
			for (const QString &w : words) {
				overlay_compat::TextFormat singleProbe(QStringLiteral("Inter, Sans Serif"), fitFs,
								       QFont::Medium);
				if (singleProbe.Measure(w).width > textR.Width() + 0.5f) {
					wordsFit = false;
					break;
				}
			}
			auto wrapped = probe.Measure(qs, textR.Width());
			if (wordsFit && wrapped.height <= textR.Height() + 1.0f)
				break;
			fitFs = std::max(minFs, fitFs - 0.65f);
		}
		overlay_compat::TextFormat fmt(QStringLiteral("Inter, Sans Serif"), fitFs, QFont::Medium);
		fmt.SetTextAlignment(overlay_compat::TextAlign::Center);
		fmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
		fmt.SetWordWrapping(overlay_compat::WordWrap::Wrap);
		c.DrawText(qs, fmt, textR, overlay_compat::ColorF(0xF8FAFC, 0.98f));
	}

	if (badgeDot) {
		c.FillEllipse({{r.right - 9.0f, r.top + 9.0f}, 3.5f, 3.5f}, activeCol);
	}
}

void OverlayRenderer::UpdateInputRegionForCurrentView()
{
	QRegion next;
	if (viewMode_ == ViewMode::HudOnly) {
		next |= QRect(static_cast<int>(std::floor(layout_.mainPanel.left)) - 2,
			      static_cast<int>(std::floor(layout_.mainPanel.top)) - 2,
			      static_cast<int>(std::ceil(layout_.mainPanel.Width())) + 4,
			      static_cast<int>(std::ceil(layout_.mainPanel.Height())) + 4);
		if (cfg_.statsVisible && layout_.statsPanel.Width() > 0) {
			next |= QRect(static_cast<int>(std::floor(layout_.statsPanel.left)) - 2,
				      static_cast<int>(std::floor(layout_.statsPanel.top)) - 2,
				      static_cast<int>(std::ceil(layout_.statsPanel.Width())) + 4,
				      static_cast<int>(std::ceil(layout_.statsPanel.Height())) + 4);
		}
		if (layout_.visibleClipCount > 0 && layout_.clipPanel.Width() > 0) {
			next |= QRect(static_cast<int>(std::floor(layout_.clipPanel.left)) - 2,
				      static_cast<int>(std::floor(layout_.clipPanel.top)) - 2,
				      static_cast<int>(std::ceil(layout_.clipPanel.Width())) + 4,
				      static_cast<int>(std::ceil(layout_.clipPanel.Height())) + 4);
		}
	} else {
		QSize sz = winMgr_.LogicalSize();
		next = QRegion(QRect(0, 0, sz.width(), sz.height()));
	}
	if (next != activeInteractiveRegion_) {
		activeInteractiveRegion_ = next;
		winMgr_.SetInputRegion(activeInteractiveRegion_, false);
	}
}

void OverlayRenderer::RenderFrame(QImage &canvas, const QRect &)
{
	hitZones_.clear();

	const QSize sz = winMgr_.LogicalSize();
	const float monW = static_cast<float>(std::max(1, sz.width()));
	const float monH = static_cast<float>(std::max(1, sz.height()));

	int visClips = 0;
	for (bool b : cfg_.clipSlotShow)
		if (b)
			++visClips;

	OverlayLayoutManager::CalculateLayout(cfg_.position, cfg_.margin, cfg_.orientation,
					      cfg_.statsVisible, cfg_.scalePercent, monW, monH,
					      visClips, layout_);

	QPainter p(&canvas);
	overlay_compat::Canvas c(&p);

	RenderMainHud(c, layout_);
	if (viewMode_ == ViewMode::Gallery)
		RenderGalleryView(c, monW, monH);
	else if (viewMode_ == ViewMode::Settings)
		RenderSettingsView(c, monW, monH);

	UpdateInputRegionForCurrentView();
}

void OverlayRenderer::RenderMainHud(overlay_compat::Canvas &c, const OverlayLayout &lay)
{
	const float s = lay.scale;
	const float ts = static_cast<float>(cfg_.textScalePercent) / 100.0f;
	const float alpha = static_cast<float>(cfg_.backgroundAlphaPercent) / 100.0f;
	const overlay_compat::ColorF accentCol(cfg_.accentRgb, 1.0f);

	// Main HUD Pill
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(lay.mainPanel, 12.0f * s, 12.0f * s),
			  overlay_compat::ColorF(0x0E1017, alpha));
	c.DrawRoundedRect(overlay_compat::MakeRoundedRect(lay.mainPanel, 12.0f * s, 12.0f * s),
			  overlay_compat::ColorF(0xFFFFFF, 0.14f), 1.0f);

	auto obsMainWinVisible = [&]() -> bool {
		if (auto *mw = static_cast<QWidget *>(obs_frontend_get_main_window()))
			return mw->isVisible();
		return true;
	};
	bool obsVis = obsMainWinVisible();

	DrawButtonTile(c, lay.toggleObsBtn, "hud_obs",
		       obs_module_text(obsVis ? "HideObs" : "ShowObs"), nullptr, !obsVis,
		       overlay_compat::ColorF(0x64748B, 1.0f), 11.5f * s * ts);
	AddHitZone("hud_obs", lay.toggleObsBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (auto *mw = static_cast<QWidget *>(obs_frontend_get_main_window())) {
			if (mw->isVisible())
				mw->hide();
			else {
				mw->show();
				mw->raise();
				mw->activateWindow();
			}
			winMgr_.RequestRepaint();
		}
	});

	DrawButtonTile(c, lay.recordBtn, "hud_rec",
		       obs_module_text(state_.recordingActive ? "Recording" : "NotRecording"), nullptr,
		       state_.recordingActive, overlay_compat::ColorF(0xEF4444, 1.0f), 11.5f * s * ts,
		       state_.recordingActive);
	AddHitZone("hud_rec", lay.recordBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
		else
			obs_frontend_recording_start();
		RefreshState();
	});

	DrawButtonTile(c, lay.replayToggleBtn, "hud_rb_toggle",
		       obs_module_text(state_.replayActive ? "Replay.TurnOff" : "Replay"), nullptr,
		       state_.replayActive, overlay_compat::ColorF(0x22C55E, 1.0f), 11.5f * s * ts,
		       state_.replayActive);
	AddHitZone("hud_rb_toggle", lay.replayToggleBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_stop();
		else
			obs_frontend_replay_buffer_start();
		RefreshState();
	});

	DrawButtonTile(c, lay.replaySaveBtn, "hud_rb_save",
		       obs_module_text(state_.replaySaving ? "Saving" : "Save"), nullptr, state_.replaySaving,
		       overlay_compat::ColorF(0xF59E0B, 1.0f), 11.5f * s * ts);
	AddHitZone("hud_rb_save", lay.replaySaveBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (obs_frontend_replay_buffer_active() && !state_.replaySaving) {
			overlay_smart_replay_note_save_request();
			obs_frontend_replay_buffer_save();
		}
	});

	DrawButtonTile(c, lay.galleryBtn, "hud_gallery", obs_module_text("Gallery"), nullptr,
		       viewMode_ == ViewMode::Gallery, accentCol, 11.5f * s * ts);
	AddHitZone("hud_gallery", lay.galleryBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (viewMode_ == ViewMode::Gallery) {
			viewMode_ = ViewMode::HudOnly;
			winMgr_.SetKeyboardInteractive(false);
		} else {
			viewMode_ = ViewMode::Gallery;
			winMgr_.SetKeyboardInteractive(true);
			RefreshGalleryList();
		}
		winMgr_.RequestRepaint();
	});

	DrawButtonTile(c, lay.settingsBtn, "hud_settings", "", &icoGear_, viewMode_ == ViewMode::Settings,
		       accentCol, 11.0f * s);
	AddHitZone("hud_settings", lay.settingsBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (viewMode_ == ViewMode::Settings) {
			viewMode_ = ViewMode::HudOnly;
			cfg_.indicatorsPreview = false;
			overlay_indicators_set_preview(false);
			winMgr_.SetKeyboardInteractive(false);
		} else {
			viewMode_ = ViewMode::Settings;
			winMgr_.SetKeyboardInteractive(true);
		}
		winMgr_.RequestRepaint();
	});

	DrawButtonTile(c, lay.statsToggleBtn, "hud_stats", "", &icoChart_, cfg_.statsVisible, accentCol,
		       11.0f * s);
	AddHitZone("hud_stats", lay.statsToggleBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		cfg_.statsVisible = !cfg_.statsVisible;
		SaveAndApplySettings();
		winMgr_.RequestRepaint();
	});

	// Stats Bar
	if (cfg_.statsVisible && lay.statsPanel.Width() > 0) {
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(lay.statsPanel, 10.0f * s, 10.0f * s),
				  overlay_compat::ColorF(0x0E1017, alpha));
		c.DrawRoundedRect(overlay_compat::MakeRoundedRect(lay.statsPanel, 10.0f * s, 10.0f * s),
				  overlay_compat::ColorF(0xFFFFFF, 0.12f), 1.0f);

		overlay_compat::TextFormat stFmt(QStringLiteral("Inter, Sans Serif"), 11.5f * s * ts,
						 QFont::Medium);
		stFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
		stFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
		stFmt.SetWordWrapping(overlay_compat::WordWrap::Wrap);

		char statsText[192];
		std::snprintf(statsText, sizeof(statsText), "%s %.1f   |   %s %d   |   %s",
			      obs_module_text("Stats.Fps"), state_.fps, obs_module_text("Stats.Dropped"),
			      state_.droppedFrames, state_.freeSpaceText.c_str());
		if (cfg_.orientation == OverlayOrientation::Vertical) {
			std::snprintf(statsText, sizeof(statsText), "%s %.1f\n%s %d\n%s",
				      obs_module_text("Stats.Fps"), state_.fps, obs_module_text("Stats.Dropped"),
				      state_.droppedFrames, state_.freeSpaceText.c_str());
		}
		auto stRect = overlay_compat::MakeRectXYWH(lay.statsPanel.left + 8.0f * s, lay.statsPanel.top + 2.0f,
							   std::max(10.0f, lay.statsPanel.Width() - 16.0f * s),
							   std::max(10.0f, lay.statsPanel.Height() - 4.0f));
		c.DrawTextUtf8(statsText, stFmt, stRect, overlay_compat::ColorF(0xCBD5E1, 0.96f));
	}

	// Timed Clip-Save Bar
	if (lay.visibleClipCount > 0 && lay.clipPanel.Width() > 0) {
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(lay.clipPanel, 9.0f * s, 9.0f * s),
				  overlay_compat::ColorF(0x0E1017, alpha));
		c.DrawRoundedRect(overlay_compat::MakeRoundedRect(lay.clipPanel, 9.0f * s, 9.0f * s),
				  overlay_compat::ColorF(0xFFFFFF, 0.12f), 1.0f);

		int btnIdx = 0;
		for (int slot = 0; slot < 4 && btnIdx < lay.visibleClipCount; ++slot) {
			if (!cfg_.clipSlotShow[slot])
				continue;
			int secs = cfg_.clipSlotSeconds[slot];
			std::string lbl = std::to_string(secs) + obs_module_text("s");
			std::string id = "hud_clip_" + std::to_string(slot);
			DrawButtonTile(c, lay.clipSlotBtns[btnIdx], id, lbl, nullptr, false, accentCol,
				       11.0f * s * ts);
			AddHitZone(id, lay.clipSlotBtns[btnIdx], [this, secs](float, float, Qt::MouseButton b) {
				if (b != Qt::LeftButton)
					return;
				overlay_clip_save_request(secs);
			});
			++btnIdx;
		}
	}
}

void OverlayRenderer::RenderGalleryView(overlay_compat::Canvas &c, float monW, float monH)
{
	const float alpha = std::max(0.88f, static_cast<float>(cfg_.backgroundAlphaPercent) / 100.0f);
	const overlay_compat::ColorF accent(cfg_.accentRgb, 1.0f);

	float margin = cfg_.galleryFullscreen ? 12.0f : 44.0f;
	auto panel = overlay_compat::MakeRectXYWH(margin, margin, monW - margin * 2.0f, monH - margin * 2.0f);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(panel, 14.0f, 14.0f),
			  overlay_compat::ColorF(0x0B0D14, alpha));
	c.DrawRoundedRect(overlay_compat::MakeRoundedRect(panel, 14.0f, 14.0f),
			  overlay_compat::ColorF(0xFFFFFF, 0.16f), 1.2f);

	// Header bar
	const float hdrH = 48.0f;
	auto hdrRect = overlay_compat::MakeRectXYWH(panel.left + 16.0f, panel.top + 10.0f,
						    panel.Width() - 32.0f, 36.0f);

	auto backBtn = overlay_compat::MakeRectXYWH(hdrRect.left, hdrRect.top, 36.0f, 36.0f);
	DrawButtonTile(c, backBtn, "gal_back", "", &icoArrowLeft_, false, accent, 12.0f);
	AddHitZone("gal_back", backBtn, [this](float, float, Qt::MouseButton) {
		viewMode_ = ViewMode::HudOnly;
		winMgr_.SetKeyboardInteractive(false);
		winMgr_.RequestRepaint();
	});

	overlay_compat::TextFormat titleFmt(QStringLiteral("Inter, Sans Serif"), 16.0f, QFont::DemiBold);
	titleFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
	c.DrawTextUtf8(obs_module_text("Gallery"), titleFmt,
		       overlay_compat::MakeRectXYWH(hdrRect.left + 48.0f, hdrRect.top, 180.0f, 36.0f),
		       overlay_compat::ColorF(0xF8FAFC, 1.0f));

	// Right header controls: Queue drawer toggle, Open Folder, Fullscreen
	auto fsBtn = overlay_compat::MakeRectXYWH(hdrRect.right - 36.0f, hdrRect.top, 36.0f, 36.0f);
	DrawButtonTile(c, fsBtn, "gal_fs", "", cfg_.galleryFullscreen ? &icoArrowsIn_ : &icoArrowsOut_,
		       cfg_.galleryFullscreen, accent, 12.0f);
	AddHitZone("gal_fs", fsBtn, [this](float, float, Qt::MouseButton) {
		cfg_.galleryFullscreen = !cfg_.galleryFullscreen;
		SaveAndApplySettings();
	});

	int qActive = exportQueue_.ActiveCount();
	int qTotal = exportQueue_.TotalCount();
	std::string qLbl = std::string(obs_module_text("Export.Queue")) + " (" + std::to_string(qTotal) + ")";
	float qW = MeasureButtonWidth(qLbl, 12.0f, 140.0f, 24.0f);
	auto qBtn = overlay_compat::MakeRectXYWH(fsBtn.left - 8.0f - qW, hdrRect.top, qW, 36.0f);
	DrawButtonTile(c, qBtn, "gal_queue_btn", qLbl, nullptr, showQueueDrawer_, accent, 12.0f, qActive > 0);
	AddHitZone("gal_queue_btn", qBtn, [this](float, float, Qt::MouseButton) {
		showQueueDrawer_ = !showQueueDrawer_;
		winMgr_.RequestRepaint();
	});

	float folderW = MeasureButtonWidth(obs_module_text("Folder"), 12.0f, 96.0f, 24.0f);
	auto folderBtn = overlay_compat::MakeRectXYWH(qBtn.left - 8.0f - folderW, hdrRect.top, folderW, 36.0f);
	DrawButtonTile(c, folderBtn, "gal_folder", obs_module_text("Folder"), nullptr, false, accent, 12.0f);
	AddHitZone("gal_folder", folderBtn, [this](float, float, Qt::MouseButton) {
		std::string dir = !selectedMediaPath_.empty() ? selectedMediaPath_
							     : OverlayStateManager::ResolveObsRecordingDirectory();
		OverlayGallery::OpenInFileManager(dir);
	});

	// Body split: left media list + right player/export workspace
	const float bodyTop = panel.top + hdrH + 14.0f;
	const float bodyH = panel.bottom - bodyTop - 14.0f;
	const float listW = std::clamp(panel.Width() * 0.28f, 240.0f, 360.0f);

	auto listRect = overlay_compat::MakeRectXYWH(panel.left + 16.0f, bodyTop, listW, bodyH);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(listRect, 10.0f, 10.0f),
			  overlay_compat::ColorF(0x111420, 0.75f));

	AddHitZone(
		"gal_list_scroll", listRect, nullptr, nullptr, nullptr, [this](int dy) {
			galleryScrollY_ = std::max(0.0f, galleryScrollY_ - static_cast<float>(dy) * 0.35f);
			winMgr_.RequestRepaint();
		});

	auto items = gallery_.SnapshotItems();
	const float cardH = 74.0f;
	const float cardGap = 8.0f;
	const float maxScroll = std::max(0.0f, items.size() * (cardH + cardGap) - listRect.Height() + 16.0f);
	galleryScrollY_ = std::clamp(galleryScrollY_, 0.0f, maxScroll);

	c.PushClip(listRect);
	if (items.empty()) {
		overlay_compat::TextFormat empFmt(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::Normal);
		empFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
		empFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
		c.DrawTextUtf8(obs_module_text("NoMediaFound"), empFmt, listRect,
			       overlay_compat::ColorF(0x64748B, 1.0f));
	} else {
		overlay_compat::TextFormat nameFmt(QStringLiteral("Inter, Sans Serif"), 12.5f, QFont::Medium);
		nameFmt.SetEllipsis(true);
		overlay_compat::TextFormat metaFmt(QStringLiteral("Inter, Sans Serif"), 11.0f, QFont::Normal);

		float cy = listRect.top + 8.0f - galleryScrollY_;
		for (size_t i = 0; i < items.size(); ++i) {
			auto cr = overlay_compat::MakeRectXYWH(listRect.left + 8.0f, cy,
							       listRect.Width() - 16.0f, cardH);
			if (cr.bottom >= listRect.top && cr.top <= listRect.bottom) {
				bool sel = (items[i].filePath == selectedMediaPath_);
				std::string zid = "gal_item_" + std::to_string(i);
				c.FillRoundedRect(
					overlay_compat::MakeRoundedRect(cr, 8.0f, 8.0f),
					sel ? overlay_compat::ColorF(accent.r, accent.g, accent.b, 0.24f)
					    : overlay_compat::ColorF(0x1B1F2E, IsHovered(zid) ? 0.9f : 0.65f));
				if (sel) {
					c.DrawRoundedRect(overlay_compat::MakeRoundedRect(cr, 8.0f, 8.0f),
							  accent, 1.3f);
				}

				auto thRect = overlay_compat::MakeRectXYWH(cr.left + 6.0f, cr.top + 6.0f,
									   108.0f, cardH - 12.0f);
				if (!items[i].thumbnail.isNull()) {
					c.DrawImage(items[i].thumbnail, thRect);
				} else {
					c.FillRoundedRect(overlay_compat::MakeRoundedRect(thRect, 5.0f, 5.0f),
							  overlay_compat::ColorF(0x0A0C12, 0.9f));
				}

				auto txtRect = overlay_compat::MakeRectXYWH(thRect.right + 8.0f, cr.top + 8.0f,
									    cr.right - thRect.right - 14.0f, 22.0f);
				c.DrawTextUtf8(items[i].fileName, nameFmt, txtRect,
					       overlay_compat::ColorF(0xF1F5F9, 1.0f));

				std::string meta = overlay_str::FormatDurationMmSs(items[i].durationSec) + " · " +
						   overlay_str::FormatFileSize(items[i].sizeBytes);
				auto mRect = overlay_compat::MakeRectXYWH(thRect.right + 8.0f, cr.top + 32.0f,
									  txtRect.Width(), 20.0f);
				c.DrawTextUtf8(meta, metaFmt, mRect, overlay_compat::ColorF(0x94A3B8, 1.0f));

				overlay_compat::RectF hitCr = cr;
				hitCr.top = std::max(hitCr.top, listRect.top);
				hitCr.bottom = std::min(hitCr.bottom, listRect.bottom);
				std::string fPath = items[i].filePath;
				AddHitZone(zid, hitCr, [this, fPath](float, float, Qt::MouseButton b) {
					if (b == Qt::LeftButton) {
						SelectMediaItem(fPath);
						winMgr_.RequestRepaint();
					}
				});
			}
			cy += cardH + cardGap;
		}
	}
	c.PopClip();

	// Right Workspace
	auto workRect = overlay_compat::MakeRectXYWH(listRect.right + 14.0f, bodyTop,
						    panel.right - 16.0f - (listRect.right + 14.0f), bodyH);

	if (showQueueDrawer_) {
		// Render Export Queue panel inside right workspace
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(workRect, 10.0f, 10.0f),
				  overlay_compat::ColorF(0x111420, 0.85f));
		auto qHdr = overlay_compat::MakeRectXYWH(workRect.left + 12.0f, workRect.top + 10.0f,
							 workRect.Width() - 24.0f, 34.0f);
		c.DrawTextUtf8(obs_module_text("Export.Queue"), titleFmt, qHdr,
			       overlay_compat::ColorF(0xF8FAFC, 1.0f));

		bool isMerge = (exportQueue_.QueueMode() == ExportQueueMode::Merge);
		float clrW = MeasureButtonWidth(obs_module_text("Export.ClearQueue"), 11.5f, 92.0f, 22.0f);
		float startQW = MeasureButtonWidth(obs_module_text("Export.Start"), 11.5f, 92.0f, 22.0f);
		std::string modeTxt = obs_module_text(isMerge ? "Export.Merge" : "Export.Separate");
		float modeW = MeasureButtonWidth(modeTxt, 11.5f, 100.0f, 22.0f);

		auto clrBtn = overlay_compat::MakeRectXYWH(qHdr.right - clrW, qHdr.top, clrW, 32.0f);
		auto startQBtn = overlay_compat::MakeRectXYWH(clrBtn.left - 8.0f - startQW, qHdr.top, startQW, 32.0f);
		auto modeBtn = overlay_compat::MakeRectXYWH(startQBtn.left - 8.0f - modeW, qHdr.top, modeW, 32.0f);

		DrawButtonTile(c, modeBtn, "q_mode", modeTxt, nullptr, isMerge, accent, 11.5f);
		AddHitZone("q_mode", modeBtn, [this, isMerge](float, float, Qt::MouseButton) {
			exportQueue_.SetQueueMode(isMerge ? ExportQueueMode::Separate : ExportQueueMode::Merge);
		});

		DrawButtonTile(c, startQBtn, "q_start", obs_module_text("Export.Start"), nullptr, true,
			       overlay_compat::ColorF(0x22C55E, 1.0f), 11.5f);
		AddHitZone("q_start", startQBtn, [this](float, float, Qt::MouseButton) {
			exportQueue_.StartQueue();
		});

		DrawButtonTile(c, clrBtn, "q_clear", obs_module_text("Export.ClearQueue"), nullptr, false,
			       overlay_compat::ColorF(0xEF4444, 1.0f), 11.5f);
		AddHitZone("q_clear", clrBtn, [this](float, float, Qt::MouseButton) {
			exportQueue_.ClearFinished();
		});

		auto jobs = exportQueue_.SnapshotJobs();
		auto qBody = overlay_compat::MakeRectXYWH(workRect.left + 12.0f, qHdr.bottom + 10.0f,
							  workRect.Width() - 24.0f,
							  workRect.bottom - qHdr.bottom - 20.0f);
		c.PushClip(qBody);
		if (jobs.empty()) {
			overlay_compat::TextFormat eFmt(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::Normal);
			eFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
			eFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
			c.DrawTextUtf8(obs_module_text("Export.QueueEmpty"), eFmt, qBody,
				       overlay_compat::ColorF(0x64748B, 1.0f));
		} else {
			overlay_compat::TextFormat jFmt(QStringLiteral("Inter, Sans Serif"), 12.5f, QFont::Medium);
			jFmt.SetEllipsis(true);
			float jy = qBody.top;
			for (size_t i = 0; i < jobs.size(); ++i) {
				const auto &j = jobs[i];
				auto jr = overlay_compat::MakeRectXYWH(qBody.left, jy, qBody.Width(), 56.0f);
				c.FillRoundedRect(overlay_compat::MakeRoundedRect(jr, 8.0f, 8.0f),
						  overlay_compat::ColorF(0x1B1F2E, 0.85f));
				c.FillRoundedRect(overlay_compat::MakeRoundedRect(
							  overlay_compat::MakeRectXYWH(jr.left + 4, jr.top + 8,
										       4, 40),
							  2, 2),
						  overlay_compat::ColorF(j.markerColorRgb, 1.0f));

				std::string stStr;
				if (j.status == ExportJobStatus::Queued)
					stStr = obs_module_text("Export.Status.Queued");
				else if (j.status == ExportJobStatus::Running)
					stStr = std::string(obs_module_text("Export.Status.Running")) + " " +
						std::to_string(static_cast<int>(j.progress * 100.0f)) + "%";
				else if (j.status == ExportJobStatus::Done)
					stStr = obs_module_text("Done");
				else if (j.status == ExportJobStatus::Failed)
					stStr = std::string(obs_module_text("Failed")) + ": " + j.errorText;
				else
					stStr = obs_module_text("Export.Status.Cancelled");

				auto rmBtn = overlay_compat::MakeRectXYWH(jr.right - 44.0f, jr.top + 12.0f,
									  36.0f, 30.0f);
				std::string rmId = "q_rm_" + std::to_string(i);
				DrawButtonTile(c, rmBtn, rmId, "", &icoTrash_, false,
					       overlay_compat::ColorF(0xEF4444, 1.0f), 11.0f);
				std::string jobId = j.id;
				AddHitZone(rmId, rmBtn, [this, jobId](float, float, Qt::MouseButton) {
					exportQueue_.RemoveJob(jobId);
				});

				float actLeft = rmBtn.left;
				if (j.status == ExportJobStatus::Failed ||
				    j.status == ExportJobStatus::Cancelled) {
					float retW = MeasureButtonWidth(obs_module_text("Export.Retry"), 11.0f, 70.0f,
									20.0f);
					auto retBtn = overlay_compat::MakeRectXYWH(rmBtn.left - 8.0f - retW,
										   jr.top + 12.0f, retW, 30.0f);
					actLeft = retBtn.left;
					std::string retId = "q_ret_" + std::to_string(i);
					DrawButtonTile(c, retBtn, retId, obs_module_text("Export.Retry"),
						       nullptr, false, accent, 11.0f);
					AddHitZone(retId, retBtn, [this, jobId](float, float, Qt::MouseButton) {
						exportQueue_.RetryJob(jobId);
					});
				} else if (j.status == ExportJobStatus::Done) {
					float opW = MeasureButtonWidth(obs_module_text("Export.OpenFolder"), 11.0f,
								       88.0f, 20.0f);
					auto opBtn = overlay_compat::MakeRectXYWH(rmBtn.left - 8.0f - opW,
										  jr.top + 12.0f, opW, 30.0f);
					actLeft = opBtn.left;
					std::string opId = "q_open_" + std::to_string(i);
					DrawButtonTile(c, opBtn, opId, obs_module_text("Export.OpenFolder"),
						       nullptr, false, accent, 11.0f);
					std::string outP = j.outputPath;
					AddHitZone(opId, opBtn, [outP](float, float, Qt::MouseButton) {
						OverlayGallery::OpenInFileManager(outP);
					});
				}

				std::string line = j.displayName + " [" +
						   overlay_str::FormatDurationMmSs(j.startSeconds) + " - " +
						   overlay_str::FormatDurationMmSs(j.endSeconds) + "] · " + stStr;
				c.DrawTextUtf8(line, jFmt,
					       overlay_compat::MakeRectXYWH(jr.left + 16, jr.top + 8,
									    std::max(60.0f, actLeft - jr.left - 24.0f),
									    38.0f),
					       overlay_compat::ColorF(0xE2E8F0, 1.0f));
				jy += 64.0f;
			}
		}
		c.PopClip();
		return;
	}

	if (!draftPresetInitialized_)
		SyncDraftFromSelectedPreset();

	// Video Player + Trimmer + Export Bar
	const float controlsBlockH = 204.0f;
	const float videoH = std::max(150.0f, workRect.Height() - controlsBlockH - 12.0f);
	auto videoBox = overlay_compat::MakeRectXYWH(workRect.left, workRect.top, workRect.Width(), videoH);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(videoBox, 10.0f, 10.0f),
			  overlay_compat::ColorF(0x05070B, 0.96f));

	QImage vFrame;
	if (player_.IsOpen() && player_.CopyCurrentFrame(vFrame) && !vFrame.isNull()) {
		double aspect = static_cast<double>(vFrame.width()) / std::max(1, vFrame.height());
		float drawW = videoBox.Width();
		float drawH = static_cast<float>(drawW / aspect);
		if (drawH > videoBox.Height()) {
			drawH = videoBox.Height();
			drawW = static_cast<float>(drawH * aspect);
		}
		float dx = videoBox.left + (videoBox.Width() - drawW) * 0.5f;
		float dy = videoBox.top + (videoBox.Height() - drawH) * 0.5f;
		c.DrawImage(vFrame, overlay_compat::MakeRectXYWH(dx, dy, drawW, drawH));
	} else {
		overlay_compat::TextFormat noFmt(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::Normal);
		noFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
		noFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
		c.DrawTextUtf8(obs_module_text("PreviewUnavailable"), noFmt, videoBox,
			       overlay_compat::ColorF(0x64748B, 1.0f));
	}

	// Playback + Timeline Controls below video
	float cy = videoBox.bottom + 10.0f;
	const double dur = std::max(0.01, player_.DurationSeconds());
	const double pos = std::clamp(player_.PositionSeconds(), 0.0, dur);

	auto tlBar = overlay_compat::MakeRectXYWH(workRect.left, cy, workRect.Width(), 22.0f);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(tlBar, 6.0f, 6.0f),
			  overlay_compat::ColorF(0x1B2030, 0.95f));
	float inFrac = static_cast<float>(std::clamp(trimStartSec_ / dur, 0.0, 1.0));
	float outFrac = static_cast<float>(std::clamp(trimEndSec_ / dur, 0.0, 1.0));
	if (outFrac <= inFrac)
		outFrac = 1.0f;
	auto rangeRect = overlay_compat::MakeRectXYWH(
		tlBar.left + inFrac * tlBar.Width(), tlBar.top + 4.0f,
		std::max(4.0f, (outFrac - inFrac) * tlBar.Width()), tlBar.Height() - 8.0f);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(rangeRect, 4.0f, 4.0f),
			  overlay_compat::ColorF(accent.r, accent.g, accent.b, 0.45f));

	float headX = tlBar.left + static_cast<float>(pos / dur) * tlBar.Width();
	c.FillRoundedRect(
		overlay_compat::MakeRoundedRect(
			overlay_compat::MakeRectXYWH(headX - 2.0f, tlBar.top, 4.0f, tlBar.Height()), 2.0f, 2.0f),
		overlay_compat::ColorF(0xFFFFFF, 0.95f));

	auto seekAtX = [this, tlBar, dur](float mx) {
		double frac = std::clamp(static_cast<double>(mx - tlBar.left) / std::max(1.0f, tlBar.Width()),
					 0.0, 1.0);
		player_.Seek(frac * dur);
	};
	AddHitZone("vid_tl", tlBar,
		   [seekAtX](float x, float, Qt::MouseButton b) {
			   if (b == Qt::LeftButton)
				   seekAtX(x);
		   },
		   [seekAtX](float x, float) { seekAtX(x); });

	cy += 30.0f;
	float bx = workRect.left;
	auto addCtrlBtn = [&](const std::string &id, const QImage *ico, const std::string &txt, float w,
			      bool act, std::function<void()> cb) {
		auto r = overlay_compat::MakeRectXYWH(bx, cy, w, 32.0f);
		DrawButtonTile(c, r, id, txt, ico, act, accent, 11.5f);
		AddHitZone(id, r, [cb](float, float, Qt::MouseButton b) {
			if (b == Qt::LeftButton)
				cb();
		});
		bx += w + 6.0f;
	};

	addCtrlBtn("vid_play", player_.IsPlaying() ? &icoPause_ : &icoPlay_, "", 34.0f, player_.IsPlaying(),
		   [this]() { player_.TogglePlayPause(); });
	addCtrlBtn("vid_step_b", &icoStepBack_, "", 32.0f, false, [this]() { player_.StepFrame(-1); });
	addCtrlBtn("vid_step_f", &icoStepFwd_, "", 32.0f, false, [this]() { player_.StepFrame(1); });
	addCtrlBtn("vid_mute", cfg_.galleryMuted ? &icoSpeakerMute_ : &icoSpeaker_, "", 32.0f,
		   cfg_.galleryMuted, [this]() {
			   cfg_.galleryMuted = !cfg_.galleryMuted;
			   SaveAndApplySettings();
		   });
	addCtrlBtn("vid_set_in", nullptr, "In", 38.0f, false, [this]() {
		trimStartSec_ = std::min(player_.PositionSeconds(), trimEndSec_ > 0 ? trimEndSec_ : player_.DurationSeconds());
		winMgr_.RequestRepaint();
	});
	addCtrlBtn("vid_set_out", nullptr, "Out", 42.0f, false, [this]() {
		double d = player_.PositionSeconds();
		if (d > trimStartSec_)
			trimEndSec_ = d;
		winMgr_.RequestRepaint();
	});

	// File quick actions on right of playback bar: Rename, Delete
	auto delBtn = overlay_compat::MakeRectXYWH(workRect.right - 34.0f, cy, 34.0f, 32.0f);
	float renW = MeasureButtonWidth(obs_module_text("Gallery.Ctx.Rename"), 11.0f, 76.0f, 20.0f);
	auto renBtn = overlay_compat::MakeRectXYWH(delBtn.left - 6.0f - renW, cy, renW, 32.0f);

	std::string timeStr = overlay_str::FormatDurationMmSs(pos) + " / " + overlay_str::FormatDurationMmSs(dur) +
			      "  [" + overlay_str::FormatDurationMmSs(trimStartSec_) + "-" +
			      overlay_str::FormatDurationMmSs(trimEndSec_) + "]";
	overlay_compat::TextFormat tFmt(QStringLiteral("Inter, Sans Serif"), 11.5f, QFont::Medium);
	tFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
	tFmt.SetEllipsis(true);
	c.DrawTextUtf8(timeStr, tFmt,
		       overlay_compat::MakeRectXYWH(bx + 6.0f, cy,
						    std::max(60.0f, renBtn.left - bx - 14.0f), 32.0f),
		       overlay_compat::ColorF(0xCBD5E1, 1.0f));

	DrawButtonTile(c, delBtn, "vid_del", "", &icoTrash_, false, overlay_compat::ColorF(0xEF4444, 1.0f),
		       11.0f);
	AddHitZone("vid_del", delBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton || selectedMediaPath_.empty())
			return;
		std::string target = selectedMediaPath_;
		ReleaseFileIfOpen(target);
		if (OverlayGallery::DeleteMediaFile(target)) {
			selectedMediaPath_.clear();
			RefreshGalleryList();
		}
		winMgr_.RequestRepaint();
	});

	DrawButtonTile(c, renBtn, "vid_ren", obs_module_text("Gallery.Ctx.Rename"), nullptr, false, accent,
		       11.0f);
	AddHitZone("vid_ren", renBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton || selectedMediaPath_.empty())
			return;
		QString stem = overlay_str::Utf8ToQString(
			std::filesystem::path(selectedMediaPath_).stem().string());
		std::string newStem = RunSavePromptText(QString::fromUtf8(obs_module_text("Gallery.Ctx.Rename")),
							QStringLiteral("Filename:"), stem);
		if (!newStem.empty()) {
			ReleaseFileIfOpen(selectedMediaPath_);
			std::string newPath, errKey;
			if (OverlayGallery::RenameMediaFile(selectedMediaPath_, newStem, newPath, errKey)) {
				selectedMediaPath_ = newPath;
				RefreshGalleryList();
				SelectMediaItem(newPath);
			}
		}
	});

	// Row 2: Preset Stepper + Mode + Save / Save As / Delete
	cy += 38.0f;
	const bool hasCopyMode = (draftPreset_.format != RebuildOutputFormat::Gif &&
				  draftPreset_.mode == RebuildMode::Copy);

	auto drawStepperTile = [&](const std::string &id, float x, float y, float w, float h,
				   const std::string &caption, const std::string &valText,
				   bool highlight, float fillFrac, std::function<void(int)> stepCb,
				   std::function<void(float)> dragRatioCb = nullptr) {
		auto r = overlay_compat::MakeRectXYWH(x, y, w, h);
		const bool hov = IsHovered(id) || IsHovered(id + "_prev") || IsHovered(id + "_next");
		overlay_compat::ColorF bg =
			highlight ? overlay_compat::ColorF(accent.r, accent.g, accent.b, hov ? 0.28f : 0.18f)
				  : overlay_compat::ColorF(0x181C2A, hov ? 0.96f : 0.82f);
		overlay_compat::ColorF br =
			highlight ? overlay_compat::ColorF(accent.r, accent.g, accent.b, 0.78f)
				  : overlay_compat::ColorF(0xFFFFFF, hov ? 0.24f : 0.11f);
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(r, 8.0f, 8.0f), bg);
		c.DrawRoundedRect(overlay_compat::MakeRoundedRect(r, 8.0f, 8.0f), br, 1.0f);

		if (fillFrac >= 0.0f) {
			float f = std::clamp(fillFrac, 0.0f, 1.0f);
			float barY = r.bottom - 4.0f;
			float innerW = std::max(1.0f, r.Width() - 48.0f);
			auto trackR = overlay_compat::MakeRectXYWH(r.left + 24.0f, barY, innerW, 2.5f);
			c.FillRoundedRect(overlay_compat::MakeRoundedRect(trackR, 1.5f, 1.5f),
					  overlay_compat::ColorF(0xFFFFFF, 0.14f));
			auto fillR = overlay_compat::MakeRectXYWH(r.left + 24.0f, barY,
								  std::max(3.0f, innerW * f), 2.5f);
			c.FillRoundedRect(overlay_compat::MakeRoundedRect(fillR, 1.5f, 1.5f), accent);
		}

		const float arrowW = 22.0f;
		auto prevR = overlay_compat::MakeRectXYWH(r.left + 2.0f, r.top + 2.0f, arrowW, h - 4.0f);
		auto nextR = overlay_compat::MakeRectXYWH(r.right - arrowW - 2.0f, r.top + 2.0f, arrowW,
							  h - 4.0f);
		overlay_compat::TextFormat arrFmt(QStringLiteral("Inter, Sans Serif"), 12.0f, QFont::DemiBold);
		arrFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
		arrFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
		c.DrawTextUtf8("<", arrFmt, prevR,
			       overlay_compat::ColorF(0xCBD5E1, IsHovered(id + "_prev") ? 1.0f : 0.72f));
		c.DrawTextUtf8(">", arrFmt, nextR,
			       overlay_compat::ColorF(0xCBD5E1, IsHovered(id + "_next") ? 1.0f : 0.72f));

		auto midR = overlay_compat::MakeRectXYWH(prevR.right + 2.0f, r.top + 2.0f,
							 std::max(20.0f, nextR.left - prevR.right - 4.0f),
							 h - (fillFrac >= 0.0f ? 7.0f : 4.0f));
		if (caption.empty()) {
			overlay_compat::TextFormat vFmt(QStringLiteral("Inter, Sans Serif"), 11.5f,
							QFont::Medium);
			vFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
			vFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
			vFmt.SetEllipsis(true);
			c.DrawTextUtf8(valText, vFmt, midR, overlay_compat::ColorF(0xF8FAFC, 0.98f));
		} else {
			overlay_compat::TextFormat capFmt(QStringLiteral("Inter, Sans Serif"), 9.5f,
							  QFont::Normal);
			capFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
			capFmt.SetEllipsis(true);
			c.DrawTextUtf8(caption, capFmt,
				       overlay_compat::MakeRectXYWH(midR.left, midR.top + 1.0f, midR.Width(),
								    13.0f),
				       overlay_compat::ColorF(0x94A3B8, 0.95f));

			overlay_compat::TextFormat vFmt(QStringLiteral("Inter, Sans Serif"), 11.5f,
							QFont::DemiBold);
			vFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
			vFmt.SetEllipsis(true);
			c.DrawTextUtf8(valText, vFmt,
				       overlay_compat::MakeRectXYWH(midR.left, midR.top + 13.0f, midR.Width(),
								    17.0f),
				       overlay_compat::ColorF(0xF8FAFC, 0.98f));
		}

		AddHitZone(
			id, midR,
			[midR, stepCb, dragRatioCb](float mx, float, Qt::MouseButton b) {
				if (b != Qt::LeftButton)
					return;
				if (dragRatioCb) {
					float ratio = std::clamp(
						(mx - midR.left) / std::max(1.0f, midR.Width()), 0.0f, 1.0f);
					dragRatioCb(ratio);
				} else if (stepCb) {
					stepCb(1);
				}
			},
			dragRatioCb ? std::function<void(float, float)>([midR, dragRatioCb](float mx, float) {
				float ratio = std::clamp((mx - midR.left) / std::max(1.0f, midR.Width()),
							 0.0f, 1.0f);
				dragRatioCb(ratio);
			})
				    : nullptr,
			nullptr,
			[stepCb](int dy) {
				if (stepCb && dy != 0)
					stepCb(dy > 0 ? -1 : 1);
			});

		AddHitZone(
			id + "_prev", prevR,
			[stepCb](float, float, Qt::MouseButton b) {
				if (b == Qt::LeftButton && stepCb)
					stepCb(-1);
			},
			nullptr, nullptr,
			[stepCb](int dy) {
				if (stepCb && dy != 0)
					stepCb(dy > 0 ? -1 : 1);
			});

		AddHitZone(
			id + "_next", nextR,
			[stepCb](float, float, Qt::MouseButton b) {
				if (b == Qt::LeftButton && stepCb)
					stepCb(1);
			},
			nullptr, nullptr,
			[stepCb](int dy) {
				if (stepCb && dy != 0)
					stepCb(dy > 0 ? -1 : 1);
			});
	};

	float r2x = workRect.left;
	std::string presetLabel =
		std::string(obs_module_text("Rebuild.Preset")) + ": " + draftPreset_.name;
	drawStepperTile(
		"exp_preset", r2x, cy, 236.0f, 32.0f, "", presetLabel, false, -1.0f,
		[this](int delta) {
			const auto &list = presetStore_.Presets();
			if (list.empty())
				return;
			int n = static_cast<int>(list.size());
			int cur = 0;
			for (int i = 0; i < n; ++i) {
				if (list[i].id == draftPreset_.id || list[i].id == presetStore_.SelectedId()) {
					cur = i;
					break;
				}
			}
			int next = ((cur + delta) % n + n) % n;
			presetStore_.SetSelectedId(list[next].id);
			SyncDraftFromSelectedPreset();
			SaveAndApplySettings();
			winMgr_.RequestRepaint();
		});
	r2x += 244.0f;

	std::string modeLabel = hasCopyMode ? "Copy" : "Reencode";
	float modeW = MeasureButtonWidth(modeLabel.c_str(), 11.5f, 92.0f, 18.0f);
	auto modeBtn = overlay_compat::MakeRectXYWH(r2x, cy, modeW, 32.0f);
	DrawButtonTile(c, modeBtn, "exp_mode", modeLabel, nullptr, !hasCopyMode, accent, 11.5f);
	AddHitZone("exp_mode", modeBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		if (draftPreset_.format == RebuildOutputFormat::Gif)
			draftPreset_.mode = RebuildMode::Reencode;
		else
			draftPreset_.mode = (draftPreset_.mode == RebuildMode::Copy) ? RebuildMode::Reencode
										     : RebuildMode::Copy;
		winMgr_.RequestRepaint();
	});
	r2x += modeW + 8.0f;

	const char *savePreLabel = obs_module_text("Rebuild.SavePreset");
	float savePreW = MeasureButtonWidth(savePreLabel, 11.0f, 88.0f, 16.0f);
	auto savePreBtn = overlay_compat::MakeRectXYWH(r2x, cy, savePreW, 32.0f);
	DrawButtonTile(c, savePreBtn, "exp_pre_save", savePreLabel, nullptr, false,
		       accent, 11.0f);
	AddHitZone("exp_pre_save", savePreBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		draftPreset_.keepVideo = keepVideo_;
		if (!presetStore_.UpdatePreset(draftPreset_))
			draftPreset_ = presetStore_.CreateCustomPreset(draftPreset_.name, draftPreset_);
		SaveAndApplySettings();
		winMgr_.RequestRepaint();
	});
	r2x += savePreW + 8.0f;

	const char *saveAsLabel = obs_module_text("Rebuild.SaveAsPreset");
	float saveAsW = MeasureButtonWidth(saveAsLabel, 11.0f, 108.0f, 16.0f);
	auto saveAsBtn = overlay_compat::MakeRectXYWH(r2x, cy, saveAsW, 32.0f);
	DrawButtonTile(c, saveAsBtn, "exp_pre_saveas", saveAsLabel, nullptr,
		       false, accent, 11.0f);
	AddHitZone("exp_pre_saveas", saveAsBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		QString initName = overlay_str::Utf8ToQString(draftPreset_.name + " Custom");
		std::string newName = RunSavePromptText(
			QString::fromUtf8(obs_module_text("Rebuild.SaveAsPreset")),
			QString::fromUtf8(obs_module_text("Rebuild.PresetName")), initName);
		if (!newName.empty()) {
			draftPreset_.keepVideo = keepVideo_;
			draftPreset_ = presetStore_.CreateCustomPreset(newName, draftPreset_);
			SaveAndApplySettings();
			winMgr_.RequestRepaint();
		}
	});
	r2x += saveAsW + 8.0f;

	const bool canDeletePreset = (presetStore_.Presets().size() > 1);
	const char *delPreLabel = obs_module_text("Rebuild.DeletePreset");
	float delPreW = MeasureButtonWidth(delPreLabel, 11.0f, 84.0f, 16.0f);
	if (r2x + delPreW > workRect.right) {
		cy += 38.0f;
		r2x = workRect.left;
	}
	auto delPreBtn = overlay_compat::MakeRectXYWH(r2x, cy, delPreW, 32.0f);
	DrawButtonTile(c, delPreBtn, "exp_pre_del", delPreLabel, nullptr, false,
		       canDeletePreset ? overlay_compat::ColorF(0xEF4444, 1.0f)
				       : overlay_compat::ColorF(0x475569, 1.0f),
		       11.0f);
	AddHitZone("exp_pre_del", delPreBtn, [this, canDeletePreset](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton || !canDeletePreset)
			return;
		if (presetStore_.DeletePreset(draftPreset_.id)) {
			SyncDraftFromSelectedPreset();
			SaveAndApplySettings();
			winMgr_.RequestRepaint();
		}
	});

	// Row 3: Format, Resolution, FPS, Quality (CRF 14..51) / GIF Colors & Dither
	cy += 38.0f;
	const float availW = workRect.Width() - 24.0f;
	const float fmtW = std::min(156.0f, availW * 0.18f);
	const float resW = std::min(196.0f, availW * 0.24f);
	const float fpsW = std::min(156.0f, availW * 0.18f);
	const float qualX = workRect.left + fmtW + resW + fpsW + 24.0f;
	const float qualW = std::max(180.0f, workRect.right - qualX);

	// Format stepper
	drawStepperTile(
		"exp_fmt", workRect.left, cy, fmtW, 38.0f, obs_module_text("Rebuild.Format"),
		draftPreset_.FormatLabel(), false, -1.0f,
		[this](int delta) {
			static constexpr RebuildOutputFormat kFmts[3] = {
				RebuildOutputFormat::Mp4, RebuildOutputFormat::Mov, RebuildOutputFormat::Gif};
			int cur = 0;
			for (int i = 0; i < 3; ++i) {
				if (kFmts[i] == draftPreset_.format) {
					cur = i;
					break;
				}
			}
			int next = ((cur + delta) % 3 + 3) % 3;
			draftPreset_.format = kFmts[next];
			if (draftPreset_.format == RebuildOutputFormat::Gif) {
				draftPreset_.mode = RebuildMode::Reencode;
				if (draftPreset_.gifFps <= 0)
					draftPreset_.gifFps = 15;
				if (draftPreset_.gifWidth <= 0)
					draftPreset_.gifWidth = 480;
			}
			winMgr_.RequestRepaint();
		});

	// Resolution stepper
	struct ResOpt {
		int w;
		int h;
		const char *label;
	};
	static constexpr ResOpt kResOpts[] = {
		{0, 0, "Source"},
		{1920, 1080, "1080p (1920x1080)"},
		{1280, 720, "720p (1280x720)"},
		{854, 480, "480p (854x480)"},
		{640, 360, "360p (640x360)"},
	};
	std::string resText = draftPreset_.format == RebuildOutputFormat::Gif
				      ? (draftPreset_.gifWidth > 0 ? std::to_string(draftPreset_.gifWidth) + "px"
								   : "480px")
				      : draftPreset_.ResolutionLabel();
	drawStepperTile(
		"exp_res", workRect.left + fmtW + 8.0f, cy, resW, 38.0f,
		obs_module_text("Rebuild.Resolution"), resText, !hasCopyMode && draftPreset_.width > 0, -1.0f,
		[this](int delta) {
			if (draftPreset_.format == RebuildOutputFormat::Gif) {
				static constexpr int kGifWidths[] = {320, 480, 640, 800};
				int cur = 1;
				for (int i = 0; i < 4; ++i) {
					if (kGifWidths[i] == draftPreset_.gifWidth) {
						cur = i;
						break;
					}
				}
				int next = ((cur + delta) % 4 + 4) % 4;
				draftPreset_.gifWidth = kGifWidths[next];
				draftPreset_.gifHeight = 0;
				winMgr_.RequestRepaint();
				return;
			}
			int cur = 0;
			for (int i = 0; i < 5; ++i) {
				if (kResOpts[i].w == draftPreset_.width && kResOpts[i].h == draftPreset_.height) {
					cur = i;
					break;
				}
			}
			int next = ((cur + delta) % 5 + 5) % 5;
			draftPreset_.width = kResOpts[next].w;
			draftPreset_.height = kResOpts[next].h;
			if (draftPreset_.mode == RebuildMode::Copy && draftPreset_.width > 0)
				draftPreset_.mode = RebuildMode::Reencode;
			winMgr_.RequestRepaint();
		});

	// FPS stepper
	std::string fpsText;
	if (draftPreset_.format == RebuildOutputFormat::Gif) {
		fpsText = std::to_string(draftPreset_.gifFps > 0 ? draftPreset_.gifFps : 15) + " fps";
	} else if (draftPreset_.fps <= 0.1) {
		fpsText = "Source";
	} else {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.0f fps", draftPreset_.fps);
		fpsText = buf;
	}
	drawStepperTile(
		"exp_fps", workRect.left + fmtW + resW + 16.0f, cy, fpsW, 38.0f,
		obs_module_text("Rebuild.Fps"), fpsText, !hasCopyMode && draftPreset_.fps > 0.1, -1.0f,
		[this](int delta) {
			if (draftPreset_.format == RebuildOutputFormat::Gif) {
				static constexpr int kGifFps[] = {10, 15, 20, 24, 30};
				int cur = 1;
				for (int i = 0; i < 5; ++i) {
					if (kGifFps[i] == draftPreset_.gifFps) {
						cur = i;
						break;
					}
				}
				int next = ((cur + delta) % 5 + 5) % 5;
				draftPreset_.gifFps = kGifFps[next];
				winMgr_.RequestRepaint();
				return;
			}
			static constexpr double kFpsOpts[] = {0.0, 120.0, 60.0, 30.0, 24.0, 15.0, 12.0};
			int cur = 0;
			for (int i = 0; i < 7; ++i) {
				if (std::abs(kFpsOpts[i] - draftPreset_.fps) < 0.2) {
					cur = i;
					break;
				}
			}
			int next = ((cur + delta) % 7 + 7) % 7;
			draftPreset_.fps = kFpsOpts[next];
			if (draftPreset_.mode == RebuildMode::Copy && draftPreset_.fps > 0.1)
				draftPreset_.mode = RebuildMode::Reencode;
			winMgr_.RequestRepaint();
		});

	// Quality (CRF 14..51 slider + stepper) OR GIF Colors & Dither
	if (draftPreset_.format == RebuildOutputFormat::Gif) {
		char gifBuf[64];
		std::snprintf(gifBuf, sizeof(gifBuf), "%d colors · dither %d", draftPreset_.gifColors,
			      draftPreset_.gifDither);
		drawStepperTile(
			"exp_gif_qual", qualX, cy, qualW, 38.0f, obs_module_text("Rebuild.Quality"),
			gifBuf, true, -1.0f,
			[this](int delta) {
				static constexpr int kColors[] = {32, 64, 96, 128, 256};
				int cur = 3;
				for (int i = 0; i < 5; ++i) {
					if (kColors[i] == draftPreset_.gifColors) {
						cur = i;
						break;
					}
				}
				int next = ((cur + delta) % 5 + 5) % 5;
				draftPreset_.gifColors = kColors[next];
				if (delta == 1) {
					// Keep colors cycling on >, cycle dither on click/wheel? Wait: > changes colors cleanly
				}
				winMgr_.RequestRepaint();
			});
	} else {
		int crf = std::clamp(draftPreset_.crf, 14, 51);
		const char *qTier = "Min";
		if (crf <= 19)
			qTier = "Max";
		else if (crf <= 23)
			qTier = "High";
		else if (crf <= 28)
			qTier = "Med";
		else if (crf <= 35)
			qTier = "Low";

		char crfBuf[64];
		std::snprintf(crfBuf, sizeof(crfBuf), "CRF %d · %s", crf, qTier);
		float crfFrac = static_cast<float>(crf - 14) / static_cast<float>(51 - 14);
		drawStepperTile(
			"exp_crf", qualX, cy, qualW, 38.0f, obs_module_text("Rebuild.Quality"), crfBuf,
			!hasCopyMode, crfFrac,
			[this](int delta) {
				draftPreset_.crf = std::clamp(draftPreset_.crf + delta, 14, 51);
				if (draftPreset_.mode == RebuildMode::Copy)
					draftPreset_.mode = RebuildMode::Reencode;
				winMgr_.RequestRepaint();
			},
			[this](float ratio) {
				draftPreset_.crf = std::clamp(
					14 + static_cast<int>(std::lround(ratio * (51 - 14))), 14, 51);
				if (draftPreset_.mode == RebuildMode::Copy)
					draftPreset_.mode = RebuildMode::Reencode;
				winMgr_.RequestRepaint();
			});
	}

	// Row 4: Video Track Toggle + Audio Track Pills + Export Start / Add to Queue
	float ax = workRect.left;
	cy += 44.0f;

	const char *addQLabel = obs_module_text("Export.AddToQueue");
	const char *startLabel = obs_module_text("Export.Start");
	float addQW = MeasureButtonWidth(addQLabel, 11.5f, 130.0f, 38.0f);
	float startW = MeasureButtonWidth(startLabel, 11.5f, 110.0f, 18.0f);
	auto addQBtn = overlay_compat::MakeRectXYWH(workRect.right - addQW, cy - 2.0f, addQW, 34.0f);
	auto startBtn = overlay_compat::MakeRectXYWH(addQBtn.left - startW - 8.0f, cy - 2.0f, startW, 34.0f);

	const char *vidLabel = obs_module_text("Export.Video");
	float vidW = MeasureButtonWidth(vidLabel, 11.0f, 84.0f, 16.0f);
	auto vidBtn = overlay_compat::MakeRectXYWH(ax, cy, vidW, 30.0f);
	DrawButtonTile(c, vidBtn, "exp_keep_video", vidLabel, nullptr, keepVideo_,
		       accent, 11.0f);
	AddHitZone("exp_keep_video", vidBtn, [this](float, float, Qt::MouseButton b) {
		if (b != Qt::LeftButton)
			return;
		keepVideo_ = !keepVideo_;
		draftPreset_.keepVideo = keepVideo_;
		winMgr_.RequestRepaint();
	});
	ax += vidW + 6.0f;

	for (const auto &as : player_.AudioStreams()) {
		bool kept = std::find(selectedAudioStreams_.begin(), selectedAudioStreams_.end(),
				      as.streamIndex) != selectedAudioStreams_.end();
		std::string lbl = as.name;
		std::string tid = "aud_trk_" + std::to_string(as.streamIndex);
		float trW = MeasureButtonWidth(lbl.c_str(), 11.0f, 88.0f, 16.0f);
		auto trR = overlay_compat::MakeRectXYWH(ax, cy, trW, 30.0f);
		if (trR.right <= startBtn.left - 8.0f) {
			DrawButtonTile(c, trR, tid, lbl, nullptr, kept, accent, 11.0f);
			int sIdx = as.streamIndex;
			AddHitZone(tid, trR, [this, sIdx, kept](float, float, Qt::MouseButton) {
				if (kept) {
					selectedAudioStreams_.erase(
						std::remove(selectedAudioStreams_.begin(),
							    selectedAudioStreams_.end(), sIdx),
						selectedAudioStreams_.end());
				} else {
					selectedAudioStreams_.push_back(sIdx);
				}
				player_.SelectAudioStream(sIdx);
				winMgr_.RequestRepaint();
			});
			ax += trW + 6.0f;
		}
	}

	auto enqueueWithDraft = [this](bool startNow) {
		if (selectedMediaPath_.empty())
			return;
		draftPreset_.keepVideo = keepVideo_;
		draftPreset_.keepAudio = !selectedAudioStreams_.empty();
		if (!draftPreset_.keepVideo && !draftPreset_.keepAudio) {
			draftPreset_.keepVideo = true;
			keepVideo_ = true;
		}
		presetStore_.UpdatePreset(draftPreset_);
		SaveAndApplySettings();

		ExportQueueJob job;
		job.inputPath = selectedMediaPath_;
		job.startSeconds = trimStartSec_;
		job.endSeconds = trimEndSec_ > trimStartSec_ ? trimEndSec_ : player_.DurationSeconds();
		job.preset = draftPreset_;
		job.outputFolder = cfg_.galleryExportPath.empty()
					   ? overlay_str::ParentDirFromPath(selectedMediaPath_)
					   : cfg_.galleryExportPath;
		job.streamIndicesToKeep = selectedAudioStreams_;
		exportQueue_.EnqueueJob(job, startNow);
		showQueueDrawer_ = true;
		winMgr_.RequestRepaint();
	};

	DrawButtonTile(c, addQBtn, "exp_add_q", obs_module_text("Export.AddToQueue"), &icoPlus_, false,
		       accent, 11.5f);
	AddHitZone("exp_add_q", addQBtn,
		   [enqueueWithDraft](float, float, Qt::MouseButton) { enqueueWithDraft(false); });

	DrawButtonTile(c, startBtn, "exp_start", obs_module_text("Export.Start"), nullptr, true,
		       overlay_compat::ColorF(0x22C55E, 1.0f), 11.5f);
	AddHitZone("exp_start", startBtn,
		   [enqueueWithDraft](float, float, Qt::MouseButton) { enqueueWithDraft(true); });
}

void OverlayRenderer::RenderSettingsView(overlay_compat::Canvas &c, float monW, float monH)
{
	const float alpha = std::max(0.90f, static_cast<float>(cfg_.backgroundAlphaPercent) / 100.0f);
	const overlay_compat::ColorF accent(cfg_.accentRgb, 1.0f);

	const float w = std::min(880.0f, monW - 64.0f);
	const float h = std::min(620.0f, monH - 64.0f);
	auto panel = overlay_compat::MakeRectXYWH((monW - w) * 0.5f, (monH - h) * 0.5f, w, h);

	c.FillRoundedRect(overlay_compat::MakeRoundedRect(panel, 14.0f, 14.0f),
			  overlay_compat::ColorF(0x0B0D14, alpha));
	c.DrawRoundedRect(overlay_compat::MakeRoundedRect(panel, 14.0f, 14.0f),
			  overlay_compat::ColorF(0xFFFFFF, 0.16f), 1.2f);

	auto hdr = overlay_compat::MakeRectXYWH(panel.left + 16.0f, panel.top + 12.0f, panel.Width() - 32.0f,
						36.0f);
	auto backBtn = overlay_compat::MakeRectXYWH(hdr.left, hdr.top, 36.0f, 36.0f);
	DrawButtonTile(c, backBtn, "set_back", "", &icoArrowLeft_, false, accent, 12.0f);
	AddHitZone("set_back", backBtn, [this](float, float, Qt::MouseButton) {
		viewMode_ = ViewMode::HudOnly;
		cfg_.indicatorsPreview = false;
		overlay_indicators_set_preview(false);
		winMgr_.SetKeyboardInteractive(false);
		winMgr_.RequestRepaint();
	});

	overlay_compat::TextFormat hdrFmt(QStringLiteral("Inter, Sans Serif"), 16.0f, QFont::DemiBold);
	hdrFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
	c.DrawTextUtf8(obs_module_text("OverlaySettings"), hdrFmt,
		       overlay_compat::MakeRectXYWH(hdr.left + 48.0f, hdr.top, 260.0f, 36.0f),
		       overlay_compat::ColorF(0xF8FAFC, 1.0f));

	// Left tabs
	const float sideW = 176.0f;
	const float bodyTop = hdr.bottom + 14.0f;
	const float bodyH = panel.bottom - bodyTop - 16.0f;
	auto sideRect = overlay_compat::MakeRectXYWH(panel.left + 16.0f, bodyTop, sideW, bodyH);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(sideRect, 10.0f, 10.0f),
			  overlay_compat::ColorF(0x111420, 0.8f));

	struct TabDef {
		SettingsTab tab;
		const char *key;
	};
	static constexpr TabDef kTabs[] = {
		{SettingsTab::General, "Tab.General"},
		{SettingsTab::Appearance, "Appearance"},
		{SettingsTab::Indicators, "Indicators"},
		{SettingsTab::Replay, "Replay"},
		{SettingsTab::ClipSave, "ClipSave"},
		{SettingsTab::Notifications, "Notifications"},
		{SettingsTab::Hotkeys, "Tab.Hotkeys"},
		{SettingsTab::About, "About"},
	};

	float ty = sideRect.top + 8.0f;
	for (const auto &td : kTabs) {
		auto tr = overlay_compat::MakeRectXYWH(sideRect.left + 8.0f, ty, sideW - 16.0f, 36.0f);
		bool act = (settingsTab_ == td.tab);
		std::string tid = "set_tab_" + std::to_string(static_cast<int>(td.tab));
		DrawButtonTile(c, tr, tid, obs_module_text(td.key), nullptr, act, accent, 12.0f);
		SettingsTab nextTab = td.tab;
		AddHitZone(tid, tr, [this, nextTab](float, float, Qt::MouseButton) {
			settingsTab_ = nextTab;
			settingsScrollY_ = 0.0f;
			winMgr_.RequestRepaint();
		});
		ty += 42.0f;
	}

	// Content pane
	auto content = overlay_compat::MakeRectXYWH(sideRect.right + 14.0f, bodyTop,
						    panel.right - 16.0f - (sideRect.right + 14.0f), bodyH);
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(content, 10.0f, 10.0f),
			  overlay_compat::ColorF(0x111420, 0.72f));

	AddHitZone("set_scroll", content, nullptr, nullptr, nullptr, [this](int dy) {
		settingsScrollY_ = std::max(0.0f, settingsScrollY_ - static_cast<float>(dy) * 0.35f);
		winMgr_.RequestRepaint();
	});

	c.PushClip(content);
	float ry = content.top + 14.0f - settingsScrollY_;
	const float rx = content.left + 16.0f;
	const float rw = content.Width() - 32.0f;
	const float rowH = 38.0f;

	overlay_compat::TextFormat lblFmt(QStringLiteral("Inter, Sans Serif"), 12.5f, QFont::Medium);
	lblFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);
	overlay_compat::TextFormat valFmt(QStringLiteral("Inter, Sans Serif"), 12.5f, QFont::Medium);
	valFmt.SetTextAlignment(overlay_compat::TextAlign::Center);
	valFmt.SetParagraphAlignment(overlay_compat::ParagraphAlign::Center);

	auto drawToggleRow = [&](const std::string &id, const char *locKey, bool &val) {
		auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text(locKey), lblFmt,
			       overlay_compat::MakeRectXYWH(rr.left, rr.top, rw - 96.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto btn = overlay_compat::MakeRectXYWH(rr.right - 84.0f, rr.top + 4.0f, 84.0f, 30.0f);
		DrawButtonTile(c, btn, id, val ? "ON" : "OFF", nullptr, val, accent, 11.5f);
		AddHitZone(id, btn, [this, &val](float, float, Qt::MouseButton) {
			val = !val;
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;
	};

	auto drawIntStepperRow = [&](const std::string &idBase, const char *locKey, int &val, int step,
				     int minV, int maxV, const std::string &suffix) {
		auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text(locKey), lblFmt,
			       overlay_compat::MakeRectXYWH(rr.left, rr.top, rw - 170.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto minusR = overlay_compat::MakeRectXYWH(rr.right - 160.0f, rr.top + 4.0f, 32.0f, 30.0f);
		auto valR = overlay_compat::MakeRectXYWH(minusR.right + 4.0f, rr.top + 4.0f, 88.0f, 30.0f);
		auto plusR = overlay_compat::MakeRectXYWH(valR.right + 4.0f, rr.top + 4.0f, 32.0f, 30.0f);

		DrawButtonTile(c, minusR, idBase + "_m", "-", nullptr, false, accent, 13.0f);
		c.DrawTextUtf8(std::to_string(val) + suffix, valFmt, valR,
			       overlay_compat::ColorF(0xF8FAFC, 1.0f));
		DrawButtonTile(c, plusR, idBase + "_p", "+", nullptr, false, accent, 13.0f);

		AddHitZone(idBase + "_m", minusR, [this, &val, step, minV, maxV](float, float, Qt::MouseButton) {
			val = std::clamp(val - step, minV, maxV);
			SaveAndApplySettings();
		});
		AddHitZone(idBase + "_p", plusR, [this, &val, step, minV, maxV](float, float, Qt::MouseButton) {
			val = std::clamp(val + step, minV, maxV);
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;
	};

	if (settingsTab_ == SettingsTab::General) {
		drawToggleRow("set_ah_en", "AutoHide.Enable", cfg_.autoHideEnabled);
		drawIntStepperRow("set_ah_sec", "AutoHide.Delay", cfg_.autoHideSeconds, 2, 2, 120,
				  obs_module_text("AutoHide.Suffix"));
		drawToggleRow("set_gal_in", "Gallery.InOverlay", cfg_.galleryInOverlay);
		drawToggleRow("set_ret_home", "ReturnHomeOnHide", cfg_.returnHomeOnHide);
		drawToggleRow("set_cap_foc", "CaptureFocus", cfg_.captureFocus);
		drawIntStepperRow("set_max_par", "Export.MaxParallel", cfg_.exportMaxParallel, 1, 1, 4, "");

		auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		std::string curExp = cfg_.galleryExportPath.empty() ? obs_module_text("Gallery.ExportPlaceholder")
								   : cfg_.galleryExportPath;
		std::string expLabel = obs_module_text("Gallery.ExportFolder");
		if (!expLabel.empty() && expLabel.back() != ':')
			expLabel.push_back(':');
		c.DrawTextUtf8(expLabel + " " + curExp, lblFmt,
			       overlay_compat::MakeRectXYWH(rr.left, rr.top, rw - 110.0f, rowH),
			       overlay_compat::ColorF(0xCBD5E1, 1.0f));
		auto brBtn = overlay_compat::MakeRectXYWH(rr.right - 100.0f, rr.top + 4.0f, 100.0f, 30.0f);
		DrawButtonTile(c, brBtn, "set_exp_browse", obs_module_text("Gallery.Browse"), nullptr, false,
			       accent, 11.5f);
		AddHitZone("set_exp_browse", brBtn, [this](float, float, Qt::MouseButton) {
			std::string dir = RunBrowseDirDialog(
				QString::fromUtf8(obs_module_text("Gallery.ExportFolder")),
				overlay_str::Utf8ToQString(cfg_.galleryExportPath));
			if (!dir.empty()) {
				cfg_.galleryExportPath = dir;
				SaveAndApplySettings();
			}
		});
		ry += rowH + 8.0f;
	} else if (settingsTab_ == SettingsTab::Appearance) {
		auto rrMon = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("Monitor.Label"), lblFmt,
			       overlay_compat::MakeRectXYWH(rrMon.left, rrMon.top, rw - 190.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		const char *modeNames[3] = {"Primary", "Specific", "Cursor"};
		auto monBtn = overlay_compat::MakeRectXYWH(rrMon.right - 180.0f, rrMon.top + 4.0f, 180.0f,
							   30.0f);
		DrawButtonTile(c, monBtn, "set_mon_mode", modeNames[static_cast<int>(cfg_.monitorMode)],
			       nullptr, false, accent, 11.5f);
		AddHitZone("set_mon_mode", monBtn, [this](float, float, Qt::MouseButton) {
			cfg_.monitorMode =
				static_cast<OverlayMonitorMode>((static_cast<int>(cfg_.monitorMode) + 1) % 3);
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;

		if (cfg_.monitorMode == OverlayMonitorMode::Specific) {
			auto mons = overlay_win::OverlayWindowManager::EnumerateMonitors();
			int maxIdx = std::max(0, static_cast<int>(mons.size()) - 1);
			drawIntStepperRow("set_mon_idx", "Monitor.Index", cfg_.monitorIndex, 1, 0, maxIdx, "");
		}

		// Position cycler
		static constexpr const char *kPosNames[9] = {"Top",    "Bottom",     "Left",
							     "Right",  "TopLeft",    "TopRight",
							     "BotLeft","BotRight",   "Center"};
		auto rrPos = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("Position.Label"), lblFmt,
			       overlay_compat::MakeRectXYWH(rrPos.left, rrPos.top, rw - 190.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto posBtn = overlay_compat::MakeRectXYWH(rrPos.right - 180.0f, rrPos.top + 4.0f, 180.0f,
							   30.0f);
		DrawButtonTile(c, posBtn, "set_pos_cycle", kPosNames[static_cast<int>(cfg_.position)], nullptr,
			       false, accent, 11.5f);
		AddHitZone("set_pos_cycle", posBtn, [this](float, float, Qt::MouseButton) {
			cfg_.position = static_cast<OverlayPosition>((static_cast<int>(cfg_.position) + 1) % 9);
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;

		auto rrOri = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("Layout"), lblFmt,
			       overlay_compat::MakeRectXYWH(rrOri.left, rrOri.top, rw - 190.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto oriBtn = overlay_compat::MakeRectXYWH(rrOri.right - 180.0f, rrOri.top + 4.0f, 180.0f,
							   30.0f);
		DrawButtonTile(c, oriBtn, "set_ori",
			       cfg_.orientation == OverlayOrientation::Horizontal ? "Horizontal" : "Vertical",
			       nullptr, false, accent, 11.5f);
		AddHitZone("set_ori", oriBtn, [this](float, float, Qt::MouseButton) {
			cfg_.orientation = (cfg_.orientation == OverlayOrientation::Horizontal)
						   ? OverlayOrientation::Vertical
						   : OverlayOrientation::Horizontal;
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;

		drawIntStepperRow("set_margin", "Margin", cfg_.margin, 4, 0, 120, " px");
		drawIntStepperRow("set_bg_alpha", "OverlayOpacity", cfg_.backgroundAlphaPercent, 5, 25, 100, "%");
		drawIntStepperRow("set_scale", "OverlayScale", cfg_.scalePercent, 10, 60, 180, "%");
		drawIntStepperRow("set_txt_sc", "TextSize", cfg_.textScalePercent, 10, 70, 150, "%");

		auto rrAcc = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("AccentColor"), lblFmt,
			       overlay_compat::MakeRectXYWH(rrAcc.left, rrAcc.top, rw - 140.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto colBtn = overlay_compat::MakeRectXYWH(rrAcc.right - 120.0f, rrAcc.top + 4.0f, 120.0f,
							   30.0f);
		DrawButtonTile(c, colBtn, "set_accent", "Pick Color", nullptr, true, accent, 11.5f);
		AddHitZone("set_accent", colBtn, [this](float, float, Qt::MouseButton) {
			uint32_t out = cfg_.accentRgb;
			if (RunPickColorDialog(cfg_.accentRgb, out)) {
				cfg_.accentRgb = out;
				SaveAndApplySettings();
			}
		});
		ry += rowH + 8.0f;
	} else if (settingsTab_ == SettingsTab::Indicators) {
		drawToggleRow("set_ind_en", "Indicators.Show", cfg_.indicatorsEnabled);
		drawToggleRow("set_ind_prev", "Indicators.Preview", cfg_.indicatorsPreview);
		drawToggleRow("set_ind_oled", "Indicators.OledProtection", cfg_.indicatorsOled);
		drawIntStepperRow("set_ind_sc", "Indicators.Scale", cfg_.indicatorsScale, 10, 50, 220, "%");

		static constexpr const char *kPosNames[9] = {"Top",    "Bottom",     "Left",
							     "Right",  "TopLeft",    "TopRight",
							     "BotLeft","BotRight",   "Center"};
		auto rrIP = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("Position.Label"), lblFmt,
			       overlay_compat::MakeRectXYWH(rrIP.left, rrIP.top, rw - 190.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto ipBtn = overlay_compat::MakeRectXYWH(rrIP.right - 180.0f, rrIP.top + 4.0f, 180.0f, 30.0f);
		DrawButtonTile(c, ipBtn, "set_ind_pos", kPosNames[static_cast<int>(cfg_.indicatorsPosition)],
			       nullptr, false, accent, 11.5f);
		AddHitZone("set_ind_pos", ipBtn, [this](float, float, Qt::MouseButton) {
			cfg_.indicatorsPosition = static_cast<OverlayPosition>(
				(static_cast<int>(cfg_.indicatorsPosition) + 1) % 9);
			SaveAndApplySettings();
		});
		ry += rowH + 10.0f;

		static constexpr const char *kSlotNames[4] = {"Recording", "Replay", "Saving", "Saved"};
		for (int i = 0; i < 4; ++i) {
			auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
			std::string cur = cfg_.indicatorIcons[i].empty()
						  ? obs_module_text("Indicators.IconPlaceholder")
						  : overlay_str::FileNameFromPath(cfg_.indicatorIcons[i]);
			const char *b1Label = obs_module_text("Indicators.Browse");
			const char *b2Label = obs_module_text("Indicators.Recolor");
			const char *b3Label = obs_module_text("Indicators.Clear");
			float b1W = MeasureButtonWidth(b1Label, 11.0f, 70.0f, 16.0f);
			float b2W = MeasureButtonWidth(b2Label, 11.0f, 64.0f, 16.0f);
			float b3W = MeasureButtonWidth(b3Label, 11.0f, 70.0f, 16.0f);
			float totalBW = b1W + b2W + b3W + 12.0f;

			c.DrawTextUtf8(std::string(kSlotNames[i]) + ": " + cur, lblFmt,
				       overlay_compat::MakeRectXYWH(rr.left, rr.top, (std::max)(80.0f, rw - totalBW - 12.0f), rowH),
				       overlay_compat::ColorF(0xCBD5E1, 1.0f));
			auto b1 = overlay_compat::MakeRectXYWH(rr.right - totalBW, rr.top + 4.0f, b1W, 30.0f);
			auto b2 = overlay_compat::MakeRectXYWH(b1.right + 6.0f, rr.top + 4.0f, b2W, 30.0f);
			auto b3 = overlay_compat::MakeRectXYWH(b2.right + 6.0f, rr.top + 4.0f, b3W, 30.0f);
			DrawButtonTile(c, b1, "ind_br_" + std::to_string(i), obs_module_text("Indicators.Browse"),
				       nullptr, false, accent, 11.0f);
			DrawButtonTile(c, b2, "ind_ti_" + std::to_string(i), obs_module_text("Indicators.Recolor"),
				       nullptr, cfg_.indicatorTints[i], accent, 11.0f);
			DrawButtonTile(c, b3, "ind_cl_" + std::to_string(i), obs_module_text("Indicators.Clear"),
				       nullptr, false, overlay_compat::ColorF(0xEF4444, 1.0f), 11.0f);
			AddHitZone("ind_br_" + std::to_string(i), b1, [this, i](float, float, Qt::MouseButton) {
				std::string f = RunBrowseFileDialog(
					QString::fromUtf8(obs_module_text("Indicators.SelectIcon")),
					QStringLiteral("Images (*.png *.svg *.jpg *.jpeg *.gif)"),
					overlay_str::Utf8ToQString(overlay_str::ParentDirFromPath(cfg_.indicatorIcons[i])));
				if (!f.empty()) {
					cfg_.indicatorIcons[i] = f;
					SaveAndApplySettings();
				}
			});
			AddHitZone("ind_ti_" + std::to_string(i), b2, [this, i](float, float, Qt::MouseButton) {
				cfg_.indicatorTints[i] = !cfg_.indicatorTints[i];
				SaveAndApplySettings();
			});
			AddHitZone("ind_cl_" + std::to_string(i), b3, [this, i](float, float, Qt::MouseButton) {
				cfg_.indicatorIcons[i].clear();
				SaveAndApplySettings();
			});
			ry += rowH + 6.0f;
		}
	} else if (settingsTab_ == SettingsTab::Replay) {
		drawToggleRow("set_rem_rb", "RememberReplayBuffer", cfg_.rememberReplayBuffer);
		drawToggleRow("set_sr_en", "SmartReplay", cfg_.smartReplayEnabled);
		drawToggleRow("set_sr_clip_ign", "SmartReplay.ClipIgnoreTrim", cfg_.smartReplayClipIgnoreTrim);

		auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
		c.DrawTextUtf8(obs_module_text("SmartReplay.Mode"), lblFmt,
			       overlay_compat::MakeRectXYWH(rr.left, rr.top, rw - 200.0f, rowH),
			       overlay_compat::ColorF(0xE2E8F0, 1.0f));
		auto mBtn = overlay_compat::MakeRectXYWH(rr.right - 190.0f, rr.top + 4.0f, 190.0f, 30.0f);
		DrawButtonTile(c, mBtn, "set_sr_mode",
			       cfg_.smartReplayMode == 0 ? "Timestamp Trim" : "Legacy Restart", nullptr, false,
			       accent, 11.5f);
		AddHitZone("set_sr_mode", mBtn, [this](float, float, Qt::MouseButton) {
			cfg_.smartReplayMode = (cfg_.smartReplayMode + 1) % 2;
			SaveAndApplySettings();
		});
		ry += rowH + 8.0f;
	} else if (settingsTab_ == SettingsTab::ClipSave) {
		for (int i = 0; i < 4; ++i) {
			std::string lbl = std::string(obs_module_text("ClipSave.Slot")) + " " +
					  std::to_string(i + 1) + " (" + std::to_string(cfg_.clipSlotSeconds[i]) +
					  "s)";
			auto rr = overlay_compat::MakeRectXYWH(rx, ry, rw, rowH);
			c.DrawTextUtf8(lbl, lblFmt,
				       overlay_compat::MakeRectXYWH(rr.left, rr.top, rw - 250.0f, rowH),
				       overlay_compat::ColorF(0xE2E8F0, 1.0f));
			auto shBtn = overlay_compat::MakeRectXYWH(rr.right - 240.0f, rr.top + 4.0f, 70.0f, 30.0f);
			auto mBtn = overlay_compat::MakeRectXYWH(shBtn.right + 6.0f, rr.top + 4.0f, 76.0f, 30.0f);
			auto pBtn = overlay_compat::MakeRectXYWH(mBtn.right + 6.0f, rr.top + 4.0f, 76.0f, 30.0f);
			DrawButtonTile(c, shBtn, "cs_sh_" + std::to_string(i), cfg_.clipSlotShow[i] ? "ON" : "OFF",
				       nullptr, cfg_.clipSlotShow[i], accent, 11.0f);
			DrawButtonTile(c, mBtn, "cs_m_" + std::to_string(i), "-5s", nullptr, false, accent, 11.0f);
			DrawButtonTile(c, pBtn, "cs_p_" + std::to_string(i), "+5s", nullptr, false, accent, 11.0f);
			AddHitZone("cs_sh_" + std::to_string(i), shBtn, [this, i](float, float, Qt::MouseButton) {
				cfg_.clipSlotShow[i] = !cfg_.clipSlotShow[i];
				SaveAndApplySettings();
			});
			AddHitZone("cs_m_" + std::to_string(i), mBtn, [this, i](float, float, Qt::MouseButton) {
				cfg_.clipSlotSeconds[i] = std::max(5, cfg_.clipSlotSeconds[i] - 5);
				SaveAndApplySettings();
			});
			AddHitZone("cs_p_" + std::to_string(i), pBtn, [this, i](float, float, Qt::MouseButton) {
				cfg_.clipSlotSeconds[i] = std::min(1800, cfg_.clipSlotSeconds[i] + 5);
				SaveAndApplySettings();
			});
			ry += rowH + 8.0f;
		}
	} else if (settingsTab_ == SettingsTab::Notifications) {
		drawToggleRow("set_nt_en", "Notifications.Enable", cfg_.toastsEnabled);
		drawToggleRow("set_nt_vs", "Notifications.VideoSaved", cfg_.toastVideoSaved);
		drawToggleRow("set_nt_ex", "Notifications.Export", cfg_.toastExport);
		drawToggleRow("set_nt_oh", "Notifications.OpenHint", cfg_.toastOpenHint);
		drawToggleRow("set_nt_ld", "Notifications.LowDisk", cfg_.toastLowDisk);
		drawIntStepperRow("set_nt_dur", "Notifications.Duration", cfg_.toastDurationSec, 1, 2, 30,
				  obs_module_text("Notifications.Seconds"));
		drawIntStepperRow("set_nt_gb", "Notifications.LowDiskThreshold", cfg_.toastLowDiskGb, 1, 1, 100,
				  obs_module_text("Notifications.GB"));
	} else if (settingsTab_ == SettingsTab::Hotkeys) {
		overlay_compat::TextFormat infoFmt(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::Normal);
		infoFmt.SetWordWrapping(overlay_compat::WordWrap::Wrap);
		c.DrawTextUtf8("Global OBS hotkeys are configured in OBS Studio -> File -> Settings -> Hotkeys:\n"
			       "• obs-plugin-overlay.toggle_overlay (Toggle Overlay)\n"
			       "• obs-plugin-overlay.save_replay (Save Replay)\n\n"
			       "While Overlay Gallery Player is open & focused:\n"
			       "• Space: Play / Pause\n"
			       "• Left / Right: Seek -5s / +5s\n"
			       "• , / .: Step Frame Back / Forward\n"
			       "• I / O: Set Trim In / Out\n"
			       "• Esc: Close Overlay View",
			       infoFmt,
			       overlay_compat::MakeRectXYWH(rx, ry, rw, 280.0f),
			       overlay_compat::ColorF(0xCBD5E1, 1.0f));
	} else if (settingsTab_ == SettingsTab::About) {
		if (!logoPng_.isNull()) {
			c.DrawImage(logoPng_, overlay_compat::MakeRectXYWH(rx, ry, 64.0f, 64.0f));
		}
		overlay_compat::TextFormat abTitle(QStringLiteral("Inter, Sans Serif"), 18.0f, QFont::Bold);
		c.DrawTextUtf8("ntchff's Overlay (Linux Wayland/X11 Port)", abTitle,
			       overlay_compat::MakeRectXYWH(rx + 80.0f, ry + 6.0f, rw - 80.0f, 30.0f),
			       overlay_compat::ColorF(0xF8FAFC, 1.0f));
		overlay_compat::TextFormat abSub(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::Normal);
		c.DrawTextUtf8(std::string(obs_module_text("About.Version")) + " 1.2.0 (Qt6 + wlr-layer-shell)",
			       abSub,
			       overlay_compat::MakeRectXYWH(rx + 80.0f, ry + 38.0f, rw - 80.0f, 24.0f),
			       overlay_compat::ColorF(0x94A3B8, 1.0f));
		ry += 88.0f;

		auto ghBtn = overlay_compat::MakeRectXYWH(rx, ry, 180.0f, 36.0f);
		DrawButtonTile(c, ghBtn, "ab_github", "GitHub", nullptr, false, accent, 12.5f);
		AddHitZone("ab_github", ghBtn, [](float, float, Qt::MouseButton) {
			QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/notch4ff4/ntchffs-overlay")));
		});
	}

	c.PopClip();
}

std::string OverlayRenderer::RunSavePromptText(const QString &title, const QString &label,
					       const QString &initial)
{
	bool wasVis = IsVisible();
	if (wasVis)
		Hide();
	bool ok = false;
	QString res = QInputDialog::getText(nullptr, title, label, QLineEdit::Normal, initial, &ok);
	if (wasVis)
		Show();
	return ok ? overlay_str::QStringToUtf8(res) : std::string{};
}

std::string OverlayRenderer::RunBrowseFileDialog(const QString &title, const QString &filter,
						 const QString &startDir)
{
	bool wasVis = IsVisible();
	if (wasVis)
		Hide();
	QString res = QFileDialog::getOpenFileName(nullptr, title, startDir, filter);
	if (wasVis)
		Show();
	return overlay_str::QStringToUtf8(res);
}

std::string OverlayRenderer::RunBrowseDirDialog(const QString &title, const QString &startDir)
{
	bool wasVis = IsVisible();
	if (wasVis)
		Hide();
	QString res = QFileDialog::getExistingDirectory(nullptr, title, startDir);
	if (wasVis)
		Show();
	return overlay_str::QStringToUtf8(res);
}

bool OverlayRenderer::RunPickColorDialog(uint32_t initialRgb, uint32_t &outRgb)
{
	bool wasVis = IsVisible();
	if (wasVis)
		Hide();
	QColor init((initialRgb >> 16) & 0xFF, (initialRgb >> 8) & 0xFF, initialRgb & 0xFF);
	QColor chosen = QColorDialog::getColor(init, nullptr, QStringLiteral("Accent Color"));
	if (wasVis)
		Show();
	if (!chosen.isValid())
		return false;
	outRgb = (static_cast<uint32_t>(chosen.red()) << 16) |
		 (static_cast<uint32_t>(chosen.green()) << 8) | static_cast<uint32_t>(chosen.blue());
	return true;
}
