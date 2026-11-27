#pragma once

#include <QImage>
#include <QObject>
#include <QRegion>
#include <QTimer>

#include <obs-data.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "overlay-export-queue.h"
#include "overlay-gallery.h"
#include "overlay-layout.h"
#include "overlay-rebuild-preset.h"
#include "overlay-state.h"
#include "overlay-video-player.h"
#include "overlay-window-manager.h"
#include "win-d2d-compat.h"

class OverlayRenderer : public QObject {
	Q_OBJECT
public:
	enum class ViewMode {
		HudOnly = 0,
		Gallery = 1,
		Settings = 2,
	};

	enum class SettingsTab {
		General = 0,
		Appearance = 1,
		Indicators = 2,
		Audio = 3,
		Replay = 4,
		ClipSave = 5,
		Notifications = 6,
		Hotkeys = 7,
		About = 8,
	};

	struct HitZone {
		std::string id;
		overlay_compat::RectF rect;
		std::function<void(float x, float y, Qt::MouseButton btn)> onPress;
		std::function<void(float x, float y)> onDrag;
		std::function<void()> onRelease;
		std::function<void(int deltaY)> onWheel;
	};

	struct OverlayConfig {
		OverlayPosition position = OverlayPosition::TopLeft;
		int margin = 20;
		OverlayOrientation orientation = OverlayOrientation::Horizontal;
		OverlayMonitorMode monitorMode = OverlayMonitorMode::Primary;
		int monitorIndex = 0;
		bool statsVisible = true;
		bool autoHideEnabled = false;
		int autoHideSeconds = 10;
		bool galleryInOverlay = true;
		bool returnHomeOnHide = true;
		bool captureFocus = false;
		int backgroundAlphaPercent = 82;
		int scalePercent = 100;
		int textScalePercent = 100;
		uint32_t accentRgb = 0xFF82A8;

		bool galleryMuted = false;
		float galleryVolume = 0.8f;
		bool galleryFullscreen = false;
		std::string galleryExportPath;
		int exportMaxParallel = 2;
		bool shareCompress = false;
		int shareCompressCrf = 26;

		std::array<int, 4> clipSlotSeconds{15, 30, 60, 120};
		std::array<bool, 4> clipSlotShow{true, true, true, false};

		// Mirrored settings for UI editing (synced to indicators/toasts/smart replay via ApplySettings)
		bool indicatorsEnabled = true;
		OverlayPosition indicatorsPosition = OverlayPosition::TopRight;
		bool indicatorsOled = false;
		int indicatorsScale = 100;
		bool indicatorsPreview = false;
		std::array<std::string, 4> indicatorIcons{};
		std::array<bool, 4> indicatorTints{false, false, false, false};
		std::array<uint32_t, 4> indicatorColors{0xEF4444, 0x22C55E, 0xF59E0B, 0x38BDF8};
		std::array<std::string, 5> indicatorSounds{};

		bool rememberReplayBuffer = false;
		bool smartReplayEnabled = false;
		int smartReplayMode = 0;
		bool smartReplayClipIgnoreTrim = false;

		bool toastsEnabled = true;
		bool toastVideoSaved = true;
		bool toastExport = true;
		bool toastOpenHint = true;
		bool toastLowDisk = true;
		int toastLowDiskGb = 5;
		int toastDurationSec = 8;
	};

	explicit OverlayRenderer(QObject *parent = nullptr);
	~OverlayRenderer() override;

	bool Initialize();
	void Shutdown();

	void Show();
	void Hide();
	void Toggle();
	bool IsVisible() const;

	void OpenSettingsView();
	void OpenFileInGallery(const std::string &filePath);
	void ReleaseFileIfOpen(const std::string &filePath);
	void OnExternalFileDeleted(const std::string &filePath);
	void NotifyReplaySaving(bool saving);

	void LoadSettings(obs_data_t *settings);
	void SaveSettings(obs_data_t *settings);
	void ApplyRuntimeSettings();

	const OverlayConfig &Config() const { return cfg_; }
	overlay_win::OverlayWindowManager *WindowManager() { return &winMgr_; }

private:
	void LoadBundledAssets();
	QImage LoadAssetImage(const std::string &relPath, int targetPx = 0);
	std::string ResolveConfigFile(const std::string &filename) const;

	void RefreshState();
	void RefreshGalleryList();
	void SelectMediaItem(const std::string &filePath);
	void SyncDraftFromSelectedPreset();
	void UpdateInputRegionForCurrentView();
	void SaveAndApplySettings();

	void RenderFrame(QImage &canvas, const QRect &dirty);
	void RenderMainHud(overlay_compat::Canvas &c, const OverlayLayout &lay);
	void RenderGalleryView(overlay_compat::Canvas &c, float monW, float monH);
	void RenderSettingsView(overlay_compat::Canvas &c, float monW, float monH);

	void DrawButtonTile(overlay_compat::Canvas &c, const overlay_compat::RectF &r, const std::string &id,
			    const std::string &label, const QImage *icon, bool active,
			    overlay_compat::ColorF activeCol, float fontSize, bool badgeDot = false);
	void AddHitZone(const std::string &id, const overlay_compat::RectF &r,
			std::function<void(float, float, Qt::MouseButton)> onPress,
			std::function<void(float, float)> onDrag = nullptr,
			std::function<void()> onRelease = nullptr,
			std::function<void(int)> onWheel = nullptr);
	bool IsHovered(const std::string &id) const { return hoveredId_ == id; }

	// Dialog helpers that safely hide the OVERLAY surface during OS modal interaction
	std::string RunSavePromptText(const QString &title, const QString &label, const QString &initial);
	std::string RunBrowseFileDialog(const QString &title, const QString &filter, const QString &startDir);
	std::string RunBrowseDirDialog(const QString &title, const QString &startDir);
	bool RunPickColorDialog(uint32_t initialRgb, uint32_t &outRgb);

	// Input handlers (overlay-renderer-input.cpp)
	void HandleMouseMove(float x, float y);
	void HandleMouseButton(float x, float y, Qt::MouseButton btn, bool pressed);
	void HandleWheel(float x, float y, int deltaY);
	void HandleKey(int qtKey, uint32_t mods, const QString &text, bool pressed);
	void TouchInteractionClock();

	overlay_win::OverlayWindowManager winMgr_;
	QTimer pollTimer_;
	QTimer animTimer_;

	OverlayConfig cfg_;
	OverlayState state_;
	OverlayLayout layout_;
	ViewMode viewMode_ = ViewMode::HudOnly;
	SettingsTab settingsTab_ = SettingsTab::General;

	OverlayGallery gallery_;
	OverlayVideoPlayer player_;
	RebuildPresetStore presetStore_;
	RebuildPreset draftPreset_;
	bool draftPresetInitialized_ = false;
	bool keepVideo_ = true;
	OverlayExportQueue exportQueue_;

	std::string selectedMediaPath_;
	double trimStartSec_ = 0.0;
	double trimEndSec_ = 0.0;
	std::vector<int> selectedAudioStreams_;
	bool showQueueDrawer_ = false;
	std::string editingJobId_;
	float galleryScrollY_ = 0.0f;
	float queueScrollY_ = 0.0f;
	float settingsScrollY_ = 0.0f;

	std::vector<HitZone> hitZones_;
	std::string hoveredId_;
	std::string activeDragId_;
	float mouseX_ = -10000.0f;
	float mouseY_ = -10000.0f;
	uint64_t lastInteractionMs_ = 0;
	bool replaySavingFlag_ = false;
	bool prevRecordingState_ = false;
	bool prevReplayState_ = false;

	QRegion activeInteractiveRegion_;

	// Bundled icons & logos
	QImage icoArrowLeft_, icoArrowsIn_, icoArrowsOut_, icoStepBack_, icoStepFwd_;
	QImage icoChart_, icoDownload_, icoGear_, icoPause_, icoPlay_, icoPlus_;
	QImage icoSpeaker_, icoSpeakerMute_, icoTrash_, logoPng_;
	QImage svgGithub_, svgTelegram_, svgTwitch_, svgDonation_;
};
