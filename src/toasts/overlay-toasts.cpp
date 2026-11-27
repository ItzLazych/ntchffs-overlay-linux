#include "overlay-toasts.h"

#include <obs-module.h>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "overlay-string-utils.h"
#include "overlay-wayland-layer.h"
#include "overlay-window-manager.h"
#include "plugin-support.h"
#include "win-d2d-compat.h"

namespace {
constexpr int kToastWidth = 340;
constexpr int kToastCardHeight = 68;
constexpr int kToastGap = 10;
constexpr uint64_t kAnimMs = 220;
} // namespace

class OverlayToasts::ToastWidget : public QWidget {
public:
	explicit ToastWidget(OverlayToasts *owner) : QWidget(nullptr), owner_(owner) {}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (!owner_ || width() <= 0 || height() <= 0)
			return;
		const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
		const int pw = std::max(1, static_cast<int>(std::lround(width() * dpr)));
		const int ph = std::max(1, static_cast<int>(std::lround(height() * dpr)));
		QImage img(pw, ph, QImage::Format_ARGB32_Premultiplied);
		img.setDevicePixelRatio(dpr);
		img.fill(Qt::transparent);
		owner_->RenderToImage(img);

		QPainter p(this);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(QPointF(0, 0), img);
	}

private:
	OverlayToasts *owner_ = nullptr;
};

OverlayToasts::OverlayToasts(QObject *parent) : QObject(parent)
{
	tickTimer_.setInterval(16);
	connect(&tickTimer_, &QTimer::timeout, this, &OverlayToasts::TickAnimations);
}

OverlayToasts::~OverlayToasts()
{
	Shutdown();
}

bool OverlayToasts::Initialize()
{
	if (widget_)
		return true;
	widget_ = new ToastWidget(this);
	return true;
}

void OverlayToasts::Shutdown()
{
	tickTimer_.stop();
	cards_.clear();
	if (widget_) {
		widget_->hide();
		widget_->deleteLater();
		widget_ = nullptr;
	}
}

void OverlayToasts::SetEnabled(bool enabled)
{
	enabled_ = enabled;
	if (!enabled_) {
		cards_.clear();
		TickAnimations();
	}
}

void OverlayToasts::SetVideoSavedEnabled(bool enabled) { videoSaved_ = enabled; }
void OverlayToasts::SetExportEnabled(bool enabled) { exportEnabled_ = enabled; }
void OverlayToasts::SetOpenHintEnabled(bool enabled) { openHintEnabled_ = enabled; }
void OverlayToasts::SetLowDiskEnabled(bool enabled) { lowDiskEnabled_ = enabled; }
void OverlayToasts::SetLowDiskThresholdGb(int gb) { lowDiskThresholdGb_ = std::clamp(gb, 1, 500); }
void OverlayToasts::SetDurationSec(int sec)
{
	durationMs_ = static_cast<uint64_t>(std::clamp(sec, 1, 60)) * 1000ULL;
}
void OverlayToasts::SetAccentColorRgb(uint32_t rgb) { accentRgb_ = rgb; }

void OverlayToasts::SetHotkeysLabels(const std::string &viewLabel, const std::string &deleteLabel)
{
	viewHotkeyLabel_ = viewLabel;
	deleteHotkeyLabel_ = deleteLabel;
}

void OverlayToasts::SetMonitor(OverlayMonitorMode mode, int monitorIndex)
{
	monitorMode_ = mode;
	monitorIndex_ = monitorIndex;
	if (widget_ && widget_->isVisible())
		RebuildGeometry();
}

void OverlayToasts::SetActionHandler(ActionHandler handler) { actionHandler_ = std::move(handler); }

void OverlayToasts::ShowReplaySavedToast(const std::string &filePath, bool /*isRecording*/)
{
	if (!enabled_ || !videoSaved_)
		return;
	ToastCard card;
	card.title = obs_module_text("Toast.ReplaySaved");
	std::string fn = overlay_str::FileNameFromPath(filePath);
	card.subtitle = fn.empty() ? filePath : fn;
	if (!viewHotkeyLabel_.empty() || !deleteHotkeyLabel_.empty()) {
		card.subtitle += "\n";
		if (!viewHotkeyLabel_.empty())
			card.subtitle += "[" + viewHotkeyLabel_ + "] ";
		if (!deleteHotkeyLabel_.empty())
			card.subtitle += "Del: " + deleteHotkeyLabel_;
	}
	card.actionPath = filePath;
	card.accentRgb = accentRgb_;
	card.hasFileActions = !filePath.empty();
	card.durationMs = durationMs_;
	PushToast(std::move(card));
}

void OverlayToasts::ShowExportToast(bool success, bool merged, const std::string &detailPathOrError)
{
	if (!enabled_ || !exportEnabled_)
		return;
	ToastCard card;
	if (success) {
		card.title = merged ? "Merge export finished" : obs_module_text("Done");
		card.subtitle = overlay_str::FileNameFromPath(detailPathOrError);
		card.actionPath = detailPathOrError;
		card.hasFileActions = !detailPathOrError.empty();
		card.accentRgb = 0x22C55E;
	} else {
		card.title = obs_module_text("Failed");
		card.subtitle = detailPathOrError;
		card.isError = true;
		card.accentRgb = 0xEF4444;
	}
	card.durationMs = durationMs_;
	PushToast(std::move(card));
}

void OverlayToasts::ShowOpenHintToast(const std::string &toggleHotkeyLabel)
{
	if (!enabled_ || !openHintEnabled_)
		return;
	ToastCard card;
	const char *fmt = obs_module_text("Toast.OpenOverlayHint");
	QString s = QString::fromUtf8(fmt ? fmt : "Press %1 to open overlay");
	if (s.contains(QLatin1String("%1")))
		s = s.arg(QString::fromUtf8(toggleHotkeyLabel.c_str()));
	else if (!toggleHotkeyLabel.empty())
		s += QLatin1String(" (") + QString::fromUtf8(toggleHotkeyLabel.c_str()) + QLatin1Char(')');

	card.title = "ntchff's Overlay";
	card.subtitle = s.toUtf8().toStdString();
	card.accentRgb = accentRgb_;
	card.durationMs = durationMs_;
	PushToast(std::move(card));
}

void OverlayToasts::CheckLowDiskSpace(uint64_t freeBytes)
{
	if (!enabled_ || !lowDiskEnabled_ || freeBytes == 0)
		return;
	uint64_t threshBytes = static_cast<uint64_t>(lowDiskThresholdGb_) * 1024ULL * 1024ULL * 1024ULL;
	if (freeBytes >= threshBytes)
		return;

	uint64_t now = overlay_compat::GetTickCount64Ms();
	if (lastLowDiskWarnMs_ != 0 && now - lastLowDiskWarnMs_ < 120000ULL)
		return;
	lastLowDiskWarnMs_ = now;

	double freeGb = static_cast<double>(freeBytes) / (1024.0 * 1024.0 * 1024.0);
	ToastCard card;
	const char *tmpl = obs_module_text("Toast.LowDiskSpace");
	QString s = QString::fromUtf8(tmpl ? tmpl : "Low disk space: %1 GB left");
	if (s.contains(QLatin1String("%1")))
		s = s.arg(QString::number(freeGb, 'f', 1));
	card.title = "ntchff's Overlay";
	card.subtitle = s.toUtf8().toStdString();
	card.isError = true;
	card.accentRgb = 0xF59E0B;
	card.durationMs = durationMs_;
	PushToast(std::move(card));
}

bool OverlayToasts::TriggerHotkeyAction(ActionKind action)
{
	std::string targetPath;
	for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
		if (!it->actionPath.empty()) {
			targetPath = it->actionPath;
			break;
		}
	}
	if (targetPath.empty())
		return false;

	if (action == ActionViewFile) {
		if (actionHandler_)
			actionHandler_(ActionViewFile, targetPath);
		return true;
	}

	if (action == ActionDeleteFile) {
		if (actionHandler_)
			actionHandler_(ActionDeleteFile, targetPath);
		std::error_code ec;
		if (std::filesystem::exists(targetPath, ec)) {
			if (std::filesystem::remove(targetPath, ec)) {
				obs_log(LOG_INFO, "Toasts: deleted replay file");
			} else {
				obs_log(LOG_WARNING, "Toasts: failed to delete replay file (%d)", ec.value());
			}
		}
		cards_.erase(std::remove_if(cards_.begin(), cards_.end(),
					    [&](const ToastCard &c) { return c.actionPath == targetPath; }),
			     cards_.end());
		TickAnimations();
		return true;
	}
	return false;
}

void OverlayToasts::PushToast(ToastCard card)
{
	if (!widget_)
		Initialize();
	card.createdMs = overlay_compat::GetTickCount64Ms();
	if (cards_.size() >= 4)
		cards_.erase(cards_.begin());
	cards_.push_back(std::move(card));

	RebuildGeometry();
	if (!widget_->isVisible())
		widget_->show();
	widget_->update();
	if (!tickTimer_.isActive())
		tickTimer_.start();
}

void OverlayToasts::RebuildGeometry()
{
	if (!widget_)
		return;
	int n = std::max(1, static_cast<int>(cards_.size()));
	int h = n * kToastCardHeight + (n - 1) * kToastGap + 16;
	int w = kToastWidth + 16;

	QScreen *targetScreen = overlay_win::OverlayWindowManager::ResolveTargetScreen(monitorMode_, monitorIndex_);

	overlay_win::LayerSurfaceSpec spec;
	spec.layer = overlay_win::LayerLevel::Overlay;
	spec.anchor = overlay_win::AnchorTop | overlay_win::AnchorRight;
	spec.margins = QMargins(0, 20, 20, 0);
	spec.exclusiveZone = -1;
	spec.keyboardMode = overlay_win::KeyboardMode::None;
	spec.scope = QStringLiteral("ntchffs-overlay-toasts");
	spec.targetScreen = targetScreen;
	spec.clickThrough = true;
	spec.desiredSize = QSize(w, h);

	widget_->setFixedSize(w, h);
	if (overlay_win::WaylandLayerHelper::IsX11Platform() && targetScreen) {
		QRect sr = targetScreen->geometry();
		widget_->move(sr.x() + sr.width() - w - 20, sr.y() + 20);
	}
	if (widget_->windowHandle() && widget_->windowHandle()->handle())
		overlay_win::WaylandLayerHelper::UpdateWidgetSpec(widget_, spec);
	else
		overlay_win::WaylandLayerHelper::ConfigureWidget(widget_, spec);
}

void OverlayToasts::TickAnimations()
{
	uint64_t now = overlay_compat::GetTickCount64Ms();
	size_t prevCount = cards_.size();
	cards_.erase(std::remove_if(cards_.begin(), cards_.end(),
				    [&](const ToastCard &c) {
					    return now - c.createdMs >= c.durationMs + kAnimMs;
				    }),
		     cards_.end());

	if (cards_.empty()) {
		if (widget_ && widget_->isVisible())
			widget_->hide();
		tickTimer_.stop();
		return;
	}

	if (cards_.size() != prevCount)
		RebuildGeometry();
	if (widget_ && widget_->isVisible())
		widget_->update();
}

void OverlayToasts::RenderToImage(QImage &canvas)
{
	uint64_t now = overlay_compat::GetTickCount64Ms();
	QPainter p(&canvas);
	overlay_compat::Canvas c(&p);

	overlay_compat::TextFormat titleFmt(QStringLiteral("Inter, Sans Serif"), 13.0f, QFont::DemiBold);
	titleFmt.SetEllipsis(true);
	overlay_compat::TextFormat subFmt(QStringLiteral("Inter, Sans Serif"), 11.0f, QFont::Normal);
	subFmt.SetWordWrapping(overlay_compat::WordWrap::Wrap);

	float y = 8.0f;
	for (const auto &card : cards_) {
		uint64_t age = now >= card.createdMs ? (now - card.createdMs) : 0;
		float alpha = 1.0f;
		float slideX = 0.0f;
		if (age < kAnimMs) {
			float t = static_cast<float>(age) / kAnimMs;
			float ease = 1.0f - std::pow(1.0f - t, 3.0f);
			alpha = ease;
			slideX = (1.0f - ease) * 32.0f;
		} else if (age > card.durationMs) {
			float t = static_cast<float>(age - card.durationMs) / kAnimMs;
			float ease = 1.0f - std::clamp(t, 0.0f, 1.0f);
			alpha = ease;
			slideX = (1.0f - ease) * 24.0f;
		}

		auto rect = overlay_compat::MakeRectXYWH(8.0f + slideX, y, kToastWidth - 8.0f, kToastCardHeight);
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(rect, 10.0f, 10.0f),
				  overlay_compat::ColorF(0x14161E, 0.92f * alpha));
		c.DrawRoundedRect(overlay_compat::MakeRoundedRect(rect, 10.0f, 10.0f),
				  overlay_compat::ColorF(0xFFFFFF, 0.14f * alpha), 1.0f);

		// Left accent bar
		auto bar = overlay_compat::MakeRectXYWH(rect.left + 3.0f, rect.top + 8.0f, 4.0f,
							kToastCardHeight - 16.0f);
		c.FillRoundedRect(overlay_compat::MakeRoundedRect(bar, 2.0f, 2.0f),
				  overlay_compat::ColorF(card.accentRgb, alpha));

		auto titleRect = overlay_compat::MakeRectXYWH(rect.left + 16.0f, rect.top + 8.0f,
							      rect.Width() - 26.0f, 20.0f);
		c.DrawTextUtf8(card.title, titleFmt, titleRect, overlay_compat::ColorF(0xF8FAFC, alpha));

		auto subRect = overlay_compat::MakeRectXYWH(rect.left + 16.0f, rect.top + 28.0f,
							    rect.Width() - 26.0f, kToastCardHeight - 34.0f);
		c.DrawTextUtf8(card.subtitle, subFmt, subRect, overlay_compat::ColorF(0x94A3B8, alpha));

		y += kToastCardHeight + kToastGap;
	}
}
