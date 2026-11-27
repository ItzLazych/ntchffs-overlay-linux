#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QRegion>
#include <QScreen>
#include <QSize>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "overlay-layout.h"
#include "overlay-wayland-layer.h"

namespace overlay_win {

struct MonitorInfo {
	int index = 0;
	QString name;
	QRect geometry;
	qreal dpr = 1.0;
	QScreen *screen = nullptr;
};

class OverlayWindowManager : public QObject {
	Q_OBJECT
public:
	using PaintCallback = std::function<void(QImage &canvas, const QRect &dirtyRect)>;
	using MouseMoveCallback = std::function<void(float x, float y)>;
	using MouseButtonCallback = std::function<void(float x, float y, Qt::MouseButton btn, bool pressed)>;
	using WheelCallback = std::function<void(float x, float y, int deltaY)>;
	using KeyCallback = std::function<void(int qtKey, uint32_t mods, const QString &text, bool pressed)>;
	using VisibilityCallback = std::function<void(bool visible)>;

	explicit OverlayWindowManager(QObject *parent = nullptr);
	~OverlayWindowManager() override;

	bool Initialize();
	void Shutdown();

	void Show();
	void Hide();
	void Toggle();
	bool IsVisible() const;

	void SetMonitorSelection(OverlayMonitorMode mode, int monitorIndex);
	QRect CurrentMonitorRect() const;
	QSize LogicalSize() const;
	qreal DevicePixelRatio() const;

	static std::vector<MonitorInfo> EnumerateMonitors();
	static QScreen *ResolveTargetScreen(OverlayMonitorMode mode, int monitorIndex);

	void RequestRepaint();
	void SetInputRegion(const QRegion &region, bool clickThrough = false);
	void SetKeyboardInteractive(bool interactive);
	void SetCaptureFocus(bool captureFocus);

	QWidget *Widget() const;

	void SetPaintCallback(PaintCallback cb) { onPaint_ = std::move(cb); }
	void SetMouseMoveCallback(MouseMoveCallback cb) { onMouseMove_ = std::move(cb); }
	void SetMouseButtonCallback(MouseButtonCallback cb) { onMouseButton_ = std::move(cb); }
	void SetWheelCallback(WheelCallback cb) { onWheel_ = std::move(cb); }
	void SetKeyCallback(KeyCallback cb) { onKey_ = std::move(cb); }
	void SetVisibilityCallback(VisibilityCallback cb) { onVisibility_ = std::move(cb); }

private:
	class SurfaceWidget;
	friend class SurfaceWidget;

	void EnsureScreenAndGeometry();
	void RecreateSurfaceOnScreen(QScreen *targetScreen);

	SurfaceWidget *widget_ = nullptr;
	OverlayMonitorMode monitorMode_ = OverlayMonitorMode::Primary;
	int monitorIndex_ = 0;
	QScreen *activeScreen_ = nullptr;
	QRegion lastInputRegion_;
	bool lastClickThrough_ = false;
	bool keyboardInteractive_ = false;
	bool captureFocus_ = false;

	PaintCallback onPaint_;
	MouseMoveCallback onMouseMove_;
	MouseButtonCallback onMouseButton_;
	WheelCallback onWheel_;
	KeyCallback onKey_;
	VisibilityCallback onVisibility_;
};

} // namespace overlay_win
