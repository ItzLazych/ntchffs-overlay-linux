#pragma once

#include <QMargins>
#include <QObject>
#include <QRegion>
#include <QSize>
#include <QString>

class QScreen;
class QWidget;
class QWindow;

namespace overlay_win {

enum LayerAnchor : uint32_t {
	AnchorNone = 0,
	AnchorTop = 1,
	AnchorBottom = 2,
	AnchorLeft = 4,
	AnchorRight = 8,
	AnchorAll = AnchorTop | AnchorBottom | AnchorLeft | AnchorRight,
};

enum class LayerLevel : uint32_t {
	Background = 0,
	Bottom = 1,
	Top = 2,
	Overlay = 3,
};

enum class KeyboardMode : uint32_t {
	None = 0,
	Exclusive = 1,
	OnDemand = 2,
};

struct LayerSurfaceSpec {
	LayerLevel layer = LayerLevel::Overlay;
	uint32_t anchor = AnchorAll;
	QSize desiredSize{0, 0};
	QMargins margins{0, 0, 0, 0};
	int32_t exclusiveZone = -1;
	KeyboardMode keyboardMode = KeyboardMode::None;
	QString scope = QStringLiteral("ntchffs-overlay");
	QScreen *targetScreen = nullptr;
	bool clickThrough = false;
	QRegion inputRegion;
};

class WaylandLayerHelper {
public:
	static bool IsWaylandPlatform();
	static bool IsX11Platform();

	// Attaches LayerSurfaceSpec tracking to a QWidget before show().
	static void ConfigureWidget(QWidget *widget, const LayerSurfaceSpec &spec);
	static void UpdateWidgetSpec(QWidget *widget, const LayerSurfaceSpec &spec);
	static void SetInputRegion(QWidget *widget, const QRegion &region, bool clickThrough);
	static void SetKeyboardMode(QWidget *widget, KeyboardMode mode);
	static void ShutdownGlobal();
};

} // namespace overlay_win
