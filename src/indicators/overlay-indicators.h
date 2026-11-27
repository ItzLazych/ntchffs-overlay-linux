#pragma once

#include <QImage>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <string>

#include "overlay-icon-frames.h"
#include "overlay-layout.h"
#include "win-d2d-compat.h"

class OverlayIndicators : public QObject {
	Q_OBJECT
public:
	static constexpr int CUSTOM_SLOT_COUNT = 4;
	static constexpr int SOUND_EVENT_COUNT = 5;

	enum CustomSlot {
		SlotRecording = 0,
		SlotReplay = 1,
		SlotSaving = 2,
		SlotSaved = 3,
	};

	enum SoundEvent {
		SoundRecordingOn = 0,
		SoundRecordingOff = 1,
		SoundReplayOn = 2,
		SoundReplayOff = 3,
		SoundReplaySaved = 4,
	};

	explicit OverlayIndicators(QObject *parent = nullptr);
	~OverlayIndicators() override;

	bool Initialize();
	void Shutdown();

	void SetEnabled(bool enabled);
	void SetPosition(OverlayPosition pos);
	void SetOledProtection(bool enabled);
	void SetScale(int scalePercent);
	void SetPreview(bool preview);
	void SetMonitor(OverlayMonitorMode mode, int monitorIndex);

	void SetCustomIcon(int slot, const std::string &path, bool tint, uint32_t colorRgb);
	void SetSoundPath(int eventIdx, const std::string &path);
	bool PlaySoundEvent(SoundEvent ev);

	void UpdateState(bool recordingActive, bool replayActive, bool replaySaving);
	void NotifyReplaySaved();

private:
	class IndicatorWidget;
	friend class IndicatorWidget;

	struct CustomSlotConfig {
		std::string path;
		bool tint = false;
		uint32_t colorRgb = 0xFFFFFF;
		overlay::icon::Slot frames;
	};

	void RebuildGeometry();
	void UpdateVisibilityAndTimer();
	void RenderToImage(QImage &canvas);
	float SlotOpacity(uint64_t nowMs, uint64_t startTickMs) const;
	uint32_t DefaultSlotColor(int slot) const;

	IndicatorWidget *widget_ = nullptr;
	QTimer animTimer_;

	bool enabled_ = true;
	OverlayPosition position_ = OverlayPosition::TopRight;
	bool oledProtection_ = false;
	int scalePercent_ = 100;
	bool previewMode_ = false;
	OverlayMonitorMode monitorMode_ = OverlayMonitorMode::Primary;
	int monitorIndex_ = 0;

	bool recordingActive_ = false;
	bool replayActive_ = false;
	bool replaySaving_ = false;
	uint64_t recStartTick_ = 0;
	uint64_t replayStartTick_ = 0;
	uint64_t savingStartTick_ = 0;
	uint64_t savedUntilTick_ = 0;
	uint64_t savedStartTick_ = 0;

	std::array<CustomSlotConfig, CUSTOM_SLOT_COUNT> slots_{};
	std::array<std::string, SOUND_EVENT_COUNT> soundPaths_{};
};
