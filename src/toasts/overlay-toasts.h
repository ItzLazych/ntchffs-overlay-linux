#pragma once

#include <QImage>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "overlay-layout.h"

class OverlayToasts : public QObject {
	Q_OBJECT
public:
	enum ActionKind {
		ActionViewFile = 0,
		ActionDeleteFile = 1,
	};

	enum AnimPhase {
		Idle = 0,
		Enter = 1,
		Hold = 2,
		Exit = 3,
	};

	using ActionHandler = std::function<void(ActionKind action, const std::string &filePath)>;

	explicit OverlayToasts(QObject *parent = nullptr);
	~OverlayToasts() override;

	bool Initialize();
	void Shutdown();

	void SetEnabled(bool enabled);
	void SetVideoSavedEnabled(bool enabled);
	void SetExportEnabled(bool enabled);
	void SetOpenHintEnabled(bool enabled);
	void SetLowDiskEnabled(bool enabled);
	void SetLowDiskThresholdGb(int gb);
	void SetDurationSec(int sec);
	void SetAccentColorRgb(uint32_t rgb);
	void SetHotkeysLabels(const std::string &viewLabel, const std::string &deleteLabel);
	void SetMonitor(OverlayMonitorMode mode, int monitorIndex);
	void SetActionHandler(ActionHandler handler);

	void ShowReplaySavedToast(const std::string &filePath, bool isRecording);
	void ShowExportToast(bool success, bool merged, const std::string &detailPathOrError);
	void ShowOpenHintToast(const std::string &toggleHotkeyLabel);
	void CheckLowDiskSpace(uint64_t freeBytes);

	bool TriggerHotkeyAction(ActionKind action);

private:
	class ToastWidget;
	friend class ToastWidget;

	struct ToastCard {
		std::string title;
		std::string subtitle;
		std::string actionPath;
		uint32_t accentRgb = 0xFF82A8;
		bool isError = false;
		bool hasFileActions = false;
		uint64_t createdMs = 0;
		uint64_t durationMs = 8000;
	};

	void PushToast(ToastCard card);
	void RebuildGeometry();
	void RenderToImage(QImage &canvas);
	void TickAnimations();

	ToastWidget *widget_ = nullptr;
	QTimer tickTimer_;
	std::vector<ToastCard> cards_;

	bool enabled_ = true;
	bool videoSaved_ = true;
	bool exportEnabled_ = true;
	bool openHintEnabled_ = true;
	bool lowDiskEnabled_ = true;
	int lowDiskThresholdGb_ = 5;
	uint64_t durationMs_ = 8000;
	uint32_t accentRgb_ = 0xFF82A8;
	std::string viewHotkeyLabel_;
	std::string deleteHotkeyLabel_;
	OverlayMonitorMode monitorMode_ = OverlayMonitorMode::Primary;
	int monitorIndex_ = 0;
	ActionHandler actionHandler_;
	uint64_t lastLowDiskWarnMs_ = 0;
};
