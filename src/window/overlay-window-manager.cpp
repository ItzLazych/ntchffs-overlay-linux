#include "overlay-window-manager.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace overlay_win {

class OverlayWindowManager::SurfaceWidget : public QWidget {
public:
	explicit SurfaceWidget(OverlayWindowManager *owner) : QWidget(nullptr), owner_(owner)
	{
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
	}

	void MarkDirty() { dirty_ = true; }

protected:
	void paintEvent(QPaintEvent *ev) override
	{
		if (!owner_ || !owner_->onPaint_)
			return;

		const QSize sz = size();
		if (sz.width() <= 0 || sz.height() <= 0)
			return;

		const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
		const int physW = std::max(1, static_cast<int>(std::lround(sz.width() * dpr)));
		const int physH = std::max(1, static_cast<int>(std::lround(sz.height() * dpr)));

		if (canvas_.width() != physW || canvas_.height() != physH) {
			canvas_ = QImage(physW, physH, QImage::Format_ARGB32_Premultiplied);
			canvas_.setDevicePixelRatio(dpr);
			dirty_ = true;
		} else if (std::abs(canvas_.devicePixelRatio() - dpr) > 0.001) {
			canvas_.setDevicePixelRatio(dpr);
			dirty_ = true;
		}

		if (dirty_) {
			canvas_.fill(Qt::transparent);
			owner_->onPaint_(canvas_, QRect(QPoint(0, 0), sz));
			dirty_ = false;
		}

		QPainter p(this);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(QPointF(0, 0), canvas_);
	}

	void mouseMoveEvent(QMouseEvent *ev) override
	{
		if (owner_ && owner_->onMouseMove_)
			owner_->onMouseMove_(static_cast<float>(ev->position().x()),
					     static_cast<float>(ev->position().y()));
		ev->accept();
	}

	void mousePressEvent(QMouseEvent *ev) override
	{
		if (owner_ && owner_->onMouseButton_)
			owner_->onMouseButton_(static_cast<float>(ev->position().x()),
					       static_cast<float>(ev->position().y()), ev->button(), true);
		ev->accept();
	}

	void mouseReleaseEvent(QMouseEvent *ev) override
	{
		if (owner_ && owner_->onMouseButton_)
			owner_->onMouseButton_(static_cast<float>(ev->position().x()),
					       static_cast<float>(ev->position().y()), ev->button(), false);
		ev->accept();
	}

	void wheelEvent(QWheelEvent *ev) override
	{
		if (owner_ && owner_->onWheel_) {
			int delta = ev->angleDelta().y();
			if (delta == 0)
				delta = ev->pixelDelta().y();
			owner_->onWheel_(static_cast<float>(ev->position().x()),
					 static_cast<float>(ev->position().y()), delta);
		}
		ev->accept();
	}

	void keyPressEvent(QKeyEvent *ev) override
	{
		if (owner_ && owner_->onKey_)
			owner_->onKey_(ev->key(), static_cast<uint32_t>(ev->modifiers()), ev->text(), true);
		ev->accept();
	}

	void keyReleaseEvent(QKeyEvent *ev) override
	{
		if (owner_ && owner_->onKey_)
			owner_->onKey_(ev->key(), static_cast<uint32_t>(ev->modifiers()), ev->text(), false);
		ev->accept();
	}

	void leaveEvent(QEvent *) override
	{
		if (owner_ && owner_->onMouseMove_)
			owner_->onMouseMove_(-10000.0f, -10000.0f);
	}

	void showEvent(QShowEvent *ev) override
	{
		QWidget::showEvent(ev);
		if (owner_ && owner_->onVisibility_)
			owner_->onVisibility_(true);
	}

	void hideEvent(QHideEvent *ev) override
	{
		QWidget::hideEvent(ev);
		if (owner_ && owner_->onVisibility_)
			owner_->onVisibility_(false);
	}

private:
	OverlayWindowManager *owner_ = nullptr;
	QImage canvas_;
	bool dirty_ = true;
};

OverlayWindowManager::OverlayWindowManager(QObject *parent) : QObject(parent) {}

OverlayWindowManager::~OverlayWindowManager()
{
	Shutdown();
}

std::vector<MonitorInfo> OverlayWindowManager::EnumerateMonitors()
{
	std::vector<MonitorInfo> out;
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i) {
		QScreen *s = screens[i];
		if (!s)
			continue;
		MonitorInfo info;
		info.index = i;
		info.name = s->name();
		info.geometry = s->geometry();
		info.dpr = s->devicePixelRatio();
		info.screen = s;
		out.push_back(std::move(info));
	}
	return out;
}

QScreen *OverlayWindowManager::ResolveTargetScreen(OverlayMonitorMode mode, int monitorIndex)
{
	const auto screens = QGuiApplication::screens();
	if (screens.isEmpty())
		return nullptr;

	if (mode == OverlayMonitorMode::Cursor) {
		if (QScreen *atCursor = QGuiApplication::screenAt(QCursor::pos()))
			return atCursor;
	} else if (mode == OverlayMonitorMode::Specific) {
		if (monitorIndex >= 0 && monitorIndex < screens.size())
			return screens[monitorIndex];
	}
	if (QScreen *primary = QGuiApplication::primaryScreen())
		return primary;
	return screens.first();
}

bool OverlayWindowManager::Initialize()
{
	if (widget_)
		return true;

	widget_ = new SurfaceWidget(this);
	QScreen *target = ResolveTargetScreen(monitorMode_, monitorIndex_);
	activeScreen_ = target;

	LayerSurfaceSpec spec;
	spec.layer = LayerLevel::Overlay;
	spec.anchor = AnchorAll;
	spec.exclusiveZone = -1;
	spec.keyboardMode = KeyboardMode::None;
	spec.scope = QStringLiteral("ntchffs-overlay-main");
	spec.targetScreen = target;
	spec.clickThrough = false;
	spec.inputRegion = lastInputRegion_;

	if (target) {
		spec.desiredSize = target->geometry().size();
		widget_->setGeometry(target->geometry());
	} else {
		spec.desiredSize = QSize(1920, 1080);
		widget_->resize(1920, 1080);
	}

	WaylandLayerHelper::ConfigureWidget(widget_, spec);
	return true;
}

void OverlayWindowManager::Shutdown()
{
	if (widget_) {
		widget_->hide();
		widget_->deleteLater();
		widget_ = nullptr;
	}
	activeScreen_ = nullptr;
}

void OverlayWindowManager::EnsureScreenAndGeometry()
{
	if (!widget_)
		return;
	QScreen *target = ResolveTargetScreen(monitorMode_, monitorIndex_);
	if (!target)
		return;

	if (target != activeScreen_) {
		RecreateSurfaceOnScreen(target);
		return;
	}

	const QRect geo = target->geometry();
	if (widget_->geometry() != geo)
		widget_->setGeometry(geo);
}

void OverlayWindowManager::RecreateSurfaceOnScreen(QScreen *targetScreen)
{
	if (!widget_)
		return;
	const bool wasVisible = widget_->isVisible();
	if (wasVisible)
		widget_->hide();

	activeScreen_ = targetScreen;
	LayerSurfaceSpec spec;
	spec.layer = LayerLevel::Overlay;
	spec.anchor = AnchorAll;
	spec.exclusiveZone = -1;
	spec.keyboardMode = keyboardInteractive_ ? KeyboardMode::Exclusive
						 : (captureFocus_ ? KeyboardMode::OnDemand : KeyboardMode::None);
	spec.scope = QStringLiteral("ntchffs-overlay-main");
	spec.targetScreen = targetScreen;
	spec.clickThrough = lastClickThrough_;
	spec.inputRegion = lastInputRegion_;

	if (targetScreen) {
		spec.desiredSize = targetScreen->geometry().size();
		widget_->setGeometry(targetScreen->geometry());
	}
	WaylandLayerHelper::ConfigureWidget(widget_, spec);

	if (wasVisible) {
		widget_->show();
		RequestRepaint();
	}
}

void OverlayWindowManager::Show()
{
	if (!widget_ && !Initialize())
		return;
	EnsureScreenAndGeometry();
	widget_->show();
	KeyboardMode mode = keyboardInteractive_ ? KeyboardMode::Exclusive
						 : (captureFocus_ ? KeyboardMode::OnDemand : KeyboardMode::None);
	WaylandLayerHelper::SetKeyboardMode(widget_, mode);
	if (captureFocus_ || keyboardInteractive_)
		widget_->activateWindow();
	WaylandLayerHelper::SetInputRegion(widget_, lastInputRegion_, lastClickThrough_);
	RequestRepaint();
}

void OverlayWindowManager::Hide()
{
	if (widget_)
		widget_->hide();
}

void OverlayWindowManager::Toggle()
{
	if (IsVisible())
		Hide();
	else
		Show();
}

bool OverlayWindowManager::IsVisible() const
{
	return widget_ && widget_->isVisible();
}

void OverlayWindowManager::SetMonitorSelection(OverlayMonitorMode mode, int monitorIndex)
{
	monitorMode_ = mode;
	monitorIndex_ = monitorIndex;
	if (widget_)
		EnsureScreenAndGeometry();
}

QRect OverlayWindowManager::CurrentMonitorRect() const
{
	if (activeScreen_)
		return QRect(QPoint(0, 0), activeScreen_->geometry().size());
	if (widget_ && widget_->screen())
		return QRect(QPoint(0, 0), widget_->screen()->geometry().size());
	return QRect(0, 0, 1920, 1080);
}

QSize OverlayWindowManager::LogicalSize() const
{
	if (widget_ && !widget_->size().isEmpty())
		return widget_->size();
	return CurrentMonitorRect().size();
}

qreal OverlayWindowManager::DevicePixelRatio() const
{
	if (widget_)
		return std::max<qreal>(1.0, widget_->devicePixelRatioF());
	if (activeScreen_)
		return std::max<qreal>(1.0, activeScreen_->devicePixelRatio());
	return 1.0;
}

QWidget *OverlayWindowManager::Widget() const
{
	return widget_;
}

void OverlayWindowManager::RequestRepaint()
{
	if (widget_) {
		widget_->MarkDirty();
		if (widget_->isVisible())
			widget_->update();
	}
}

void OverlayWindowManager::SetInputRegion(const QRegion &region, bool clickThrough)
{
	lastInputRegion_ = region;
	lastClickThrough_ = clickThrough;
	if (widget_)
		WaylandLayerHelper::SetInputRegion(widget_, region, clickThrough);
}

void OverlayWindowManager::SetKeyboardInteractive(bool interactive)
{
	if (keyboardInteractive_ == interactive)
		return;
	keyboardInteractive_ = interactive;
	if (widget_) {
		KeyboardMode mode = keyboardInteractive_
					    ? KeyboardMode::Exclusive
					    : (captureFocus_ ? KeyboardMode::OnDemand : KeyboardMode::None);
		WaylandLayerHelper::SetKeyboardMode(widget_, mode);
		if (interactive && widget_->isVisible())
			widget_->activateWindow();
	}
}

void OverlayWindowManager::SetCaptureFocus(bool captureFocus)
{
	if (captureFocus_ == captureFocus)
		return;
	captureFocus_ = captureFocus;
	if (widget_) {
		KeyboardMode mode = keyboardInteractive_
					    ? KeyboardMode::Exclusive
					    : (captureFocus_ ? KeyboardMode::OnDemand : KeyboardMode::None);
		WaylandLayerHelper::SetKeyboardMode(widget_, mode);
	}
}

} // namespace overlay_win
