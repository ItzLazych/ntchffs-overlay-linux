#include "overlay-wayland-layer.h"

#include "plugin-support.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <mutex>
#include <unordered_map>

#include <wayland-client.h>

#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtWaylandClient/private/qwaylandscreen_p.h>
#include <QtWaylandClient/private/qwaylandshellintegration_p.h>
#include <QtWaylandClient/private/qwaylandshellsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include "qwayland-wlr-layer-shell-unstable-v1.h"

// Include X11 headers after Qt headers to avoid macro collisions
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef Bool
#undef Bool
#endif

namespace overlay_win {

namespace {

class WlrLayerShellIntegration;
class WlrLayerSurface;

struct WindowEntry {
	LayerSurfaceSpec spec;
	WlrLayerSurface *surface = nullptr;
};

static QHash<QWindow *, WindowEntry> &WindowRegistry()
{
	static QHash<QWindow *, WindowEntry> reg;
	return reg;
}

static std::mutex g_surfaceMutex;
static std::unordered_map<QWindow *, WlrLayerSurface *> g_activeSurfaces;

class WlrLayerSurface : public QtWaylandClient::QWaylandShellSurface,
			public QtWayland::zwlr_layer_surface_v1 {
public:
	WlrLayerSurface(QtWayland::zwlr_layer_shell_v1 *shell, QtWaylandClient::QWaylandWindow *window,
			const LayerSurfaceSpec &spec)
		: QtWaylandClient::QWaylandShellSurface(window), spec_(spec)
	{
		if (QWindow *qwin = window ? window->window() : nullptr) {
			std::lock_guard<std::mutex> lock(g_surfaceMutex);
			g_activeSurfaces[qwin] = this;
		}

		struct ::wl_output *wlOut = nullptr;
		if (spec_.targetScreen) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
			if (auto *native = spec_.targetScreen->nativeInterface<QNativeInterface::QWaylandScreen>())
				wlOut = native->output();
#else
			if (auto *ws = dynamic_cast<QtWaylandClient::QWaylandScreen *>(spec_.targetScreen->handle()))
				wlOut = ws->output();
#endif
		}
		if (!wlOut && window->waylandScreen())
			wlOut = window->waylandScreen()->output();

		::zwlr_layer_surface_v1 *raw = shell->get_layer_surface(
			window->wlSurface(), wlOut, static_cast<uint32_t>(spec_.layer), spec_.scope);
		init(raw);

		obs_log(LOG_INFO, "Wayland layer surface created scope=%s layer=%u anchor=%u size=%dx%d",
			spec_.scope.toUtf8().constData(), static_cast<unsigned>(spec_.layer),
			spec_.anchor, spec_.desiredSize.width(), spec_.desiredSize.height());

		ApplySpecToProtocol(true);
		wl_surface_commit(window->wlSurface());
	}

	~WlrLayerSurface() override
	{
		if (auto *qwin = window() ? window()->window() : nullptr) {
			{
				std::lock_guard<std::mutex> lock(g_surfaceMutex);
				auto itSurf = g_activeSurfaces.find(qwin);
				if (itSurf != g_activeSurfaces.end() && itSurf->second == this)
					g_activeSurfaces.erase(itSurf);
			}
			auto it = WindowRegistry().find(qwin);
			if (it != WindowRegistry().end() && it->surface == this)
				it->surface = nullptr;
		}
		if (isInitialized())
			destroy();
	}

	bool isExposed() const override { return configured_; }

	void applyConfigure() override
	{
		configured_ = true;
		if (pendingSerial_ != 0 && pendingSerial_ != appliedSerial_) {
			ack_configure(pendingSerial_);
			appliedSerial_ = pendingSerial_;
		}
		resizeFromApplyConfigure(pendingSize_);
		ApplyInputRegion();
		if (auto *w = window())
			w->updateExposure();
	}

	void UpdateSpec(const LayerSurfaceSpec &spec)
	{
		spec_ = spec;
		if (isInitialized()) {
			ApplySpecToProtocol(false);
			ApplyInputRegion();
			if (auto *w = window()) {
				if (auto *surf = w->wlSurface())
					wl_surface_commit(surf);
				if (auto *disp = w->display(); disp && disp->wl_display())
					wl_display_flush(disp->wl_display());
			}
		}
	}

	void updateKeyboardMode(KeyboardMode mode)
	{
		spec_.keyboardMode = mode;
		if (isInitialized()) {
			set_keyboard_interactivity(static_cast<uint32_t>(mode));
			if (auto *w = window()) {
				if (auto *surf = w->wlSurface())
					wl_surface_commit(surf);
				if (auto *disp = w->display(); disp && disp->wl_display())
					wl_display_flush(disp->wl_display());
			}
		}
	}

	void ApplyInputRegion()
	{
		auto *wlcWin = window();
		if (!wlcWin)
			return;
		struct ::wl_surface *surf = wlcWin->wlSurface();
		auto *disp = wlcWin->display();
		if (!surf || !disp)
			return;

		QRegion targetRegion = spec_.clickThrough ? QRegion() : spec_.inputRegion;
		if (!spec_.clickThrough && targetRegion.isEmpty()) {
			// Non-click-through with empty custom region -> accept entire surface
			wl_surface_set_input_region(surf, nullptr);
		} else {
			struct ::wl_region *wlReg = disp->createRegion(targetRegion);
			if (wlReg) {
				wl_surface_set_input_region(surf, wlReg);
				wl_region_destroy(wlReg);
			}
		}
		wl_surface_commit(surf);
	}

protected:
	void zwlr_layer_surface_v1_configure(uint32_t serial, uint32_t width, uint32_t height) override
	{
		int fallbackW = spec_.desiredSize.width() > 0 ? spec_.desiredSize.width() : 1920;
		int fallbackH = spec_.desiredSize.height() > 0 ? spec_.desiredSize.height() : 1080;
		int w = width > 0 ? static_cast<int>(width) : fallbackW;
		int h = height > 0 ? static_cast<int>(height) : fallbackH;
		pendingSize_ = QSize(w, h);
		pendingSerial_ = serial;

		obs_log(LOG_INFO, "Wayland layer surface configured scope=%s %dx%d (serial=%u, initial=%s)",
			spec_.scope.toUtf8().constData(), w, h, serial, configured_ ? "no" : "yes");

		if (!configured_)
			applyConfigure();
		else
			applyConfigureWhenPossible();
	}

	void zwlr_layer_surface_v1_closed() override
	{
		configured_ = false;
		if (window() && window()->window())
			window()->window()->close();
	}

private:
	void ApplySpecToProtocol(bool initial)
	{
		if (initial)
			set_layer(static_cast<uint32_t>(spec_.layer));
		else
			set_layer(static_cast<uint32_t>(spec_.layer));

		uint32_t w = (spec_.anchor & (AnchorLeft | AnchorRight)) == (AnchorLeft | AnchorRight)
				     ? 0
				     : static_cast<uint32_t>(std::max(1, spec_.desiredSize.width()));
		uint32_t h = (spec_.anchor & (AnchorTop | AnchorBottom)) == (AnchorTop | AnchorBottom)
				     ? 0
				     : static_cast<uint32_t>(std::max(1, spec_.desiredSize.height()));
		if ((spec_.anchor & AnchorAll) == 0 && spec_.desiredSize.width() > 0 &&
		    spec_.desiredSize.height() > 0) {
			w = static_cast<uint32_t>(spec_.desiredSize.width());
			h = static_cast<uint32_t>(spec_.desiredSize.height());
		}

		set_anchor(spec_.anchor);
		set_size(w, h);
		set_exclusive_zone(spec_.exclusiveZone);
		set_margin(spec_.margins.top(), spec_.margins.right(), spec_.margins.bottom(),
			   spec_.margins.left());
		set_keyboard_interactivity(static_cast<uint32_t>(spec_.keyboardMode));
	}

	LayerSurfaceSpec spec_;
	QSize pendingSize_{64, 64};
	uint32_t pendingSerial_ = 0;
	uint32_t appliedSerial_ = 0;
	bool configured_ = false;
};

class WlrLayerShellIntegration : public QtWaylandClient::QWaylandShellIntegration {
public:
	bool initialize(QtWaylandClient::QWaylandDisplay *display) override
	{
		if (shell_ && shell_->isInitialized())
			return true;
		if (!display)
			return false;
		display_ = display;
		display->addRegistryListener(&WlrLayerShellIntegration::RegistryCallback, this);
		if (shell_ && shell_->isInitialized())
			return true;
		// Also inspect already-discovered globals
		for (const auto &g : display->globals()) {
			if (g.interface == QLatin1String("zwlr_layer_shell_v1")) {
				BindShell(g.registry, g.id, g.version);
				break;
			}
		}
		if (!shell_ || !shell_->isInitialized())
			display->forceRoundTrip();
		return shell_ && shell_->isInitialized();
	}

	QtWaylandClient::QWaylandShellSurface *createShellSurface(QtWaylandClient::QWaylandWindow *window) override
	{
		if (!window)
			return nullptr;
		if (!shell_ || !shell_->isInitialized())
			initialize(window->display());
		if (!shell_ || !shell_->isInitialized())
			return nullptr;

		QWindow *qwin = window->window();
		LayerSurfaceSpec spec;
		if (qwin && WindowRegistry().contains(qwin))
			spec = WindowRegistry()[qwin].spec;

		auto *surf = new WlrLayerSurface(shell_.get(), window, spec);
		if (qwin)
			WindowRegistry()[qwin].surface = surf;
		return surf;
	}

	void Reset()
	{
		if (display_) {
			display_->removeListener(&WlrLayerShellIntegration::RegistryCallback, this);
			display_ = nullptr;
		}
		if (shell_ && shell_->isInitialized())
			shell_->destroy();
		shell_.reset();
	}

	bool HasShell() const { return shell_ && shell_->isInitialized(); }

private:
	static void RegistryCallback(void *data, struct wl_registry *registry, uint32_t id,
				     const QString &interface, uint32_t version)
	{
		auto *self = static_cast<WlrLayerShellIntegration *>(data);
		if (!self)
			return;
		if (interface == QLatin1String("zwlr_layer_shell_v1"))
			self->BindShell(registry, id, version);
	}

	void BindShell(struct wl_registry *registry, uint32_t id, uint32_t version)
	{
		if (shell_ && shell_->isInitialized())
			return;
		uint32_t v = std::min<uint32_t>(version, 4);
		shell_ = std::make_unique<QtWayland::zwlr_layer_shell_v1>(registry, id, static_cast<int>(v));
	}

	QtWaylandClient::QWaylandDisplay *display_ = nullptr;
	std::unique_ptr<QtWayland::zwlr_layer_shell_v1> shell_;
};

static WlrLayerShellIntegration *GetGlobalShellIntegration()
{
	static WlrLayerShellIntegration inst;
	return &inst;
}

class LayerSurfaceEventFilter : public QObject {
public:
	explicit LayerSurfaceEventFilter(QObject *parent = nullptr) : QObject(parent) {}

	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		auto *qwin = qobject_cast<QWindow *>(obj);
		if (!qwin)
			return QObject::eventFilter(obj, ev);

		if (ev->type() == QEvent::PlatformSurface) {
			auto *pe = static_cast<QPlatformSurfaceEvent *>(ev);
			if (pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
				AttachShellIntegration(qwin);
			} else if (pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				auto it = WindowRegistry().find(qwin);
				if (it != WindowRegistry().end())
					it->surface = nullptr;
			}
		} else if (ev->type() == QEvent::Destroy) {
			WindowRegistry().remove(qwin);
		}
		return QObject::eventFilter(obj, ev);
	}

	static void AttachShellIntegration(QWindow *qwin)
	{
		if (!qwin || !WaylandLayerHelper::IsWaylandPlatform())
			return;
		auto *wlcWin = dynamic_cast<QtWaylandClient::QWaylandWindow *>(qwin->handle());
		if (!wlcWin) {
			obs_log(LOG_WARNING, "AttachShellIntegration: QWindow has no QWaylandWindow handle");
			return;
		}
		if (wlcWin->shellSurface())
			return;
		auto *integ = GetGlobalShellIntegration();
		if (integ->initialize(wlcWin->display()) && integ->HasShell()) {
			wlcWin->setShellIntegration(integ);
		} else {
			obs_log(LOG_WARNING, "AttachShellIntegration: zwlr_layer_shell_v1 not available on compositor");
		}
	}
};

static LayerSurfaceEventFilter *GetGlobalEventFilter()
{
	static LayerSurfaceEventFilter filter;
	return &filter;
}

void ApplyWaylandToplevelInputRegion(QWidget *widget, const QRegion &region, bool clickThrough)
{
	if (!widget || !WaylandLayerHelper::IsWaylandPlatform())
		return;
	QWindow *qwin = widget->windowHandle();
	if (!qwin)
		return;
	auto *wlcWin = dynamic_cast<QtWaylandClient::QWaylandWindow *>(qwin->handle());
	if (!wlcWin)
		return;
	struct ::wl_surface *surf = wlcWin->wlSurface();
	auto *disp = wlcWin->display();
	if (!surf || !disp)
		return;

	QRegion targetRegion = clickThrough ? QRegion() : region;
	if (!clickThrough && targetRegion.isEmpty()) {
		wl_surface_set_input_region(surf, nullptr);
	} else {
		struct ::wl_region *wlReg = disp->createRegion(targetRegion);
		if (wlReg) {
			wl_surface_set_input_region(surf, wlReg);
			wl_region_destroy(wlReg);
		}
	}
	wl_surface_commit(surf);
}

void ApplyX11InputShape(QWidget *widget, const QRegion &region, bool clickThrough)
{
	if (!widget || !WaylandLayerHelper::IsX11Platform())
		return;
	auto *x11App = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
	if (!x11App)
		return;
	Display *dpy = x11App->display();
	if (!dpy || !widget->windowHandle())
		return;

	Window xwin = static_cast<Window>(widget->winId());
	if (!xwin)
		return;

	qreal dpr = widget->devicePixelRatioF();
	if (clickThrough) {
		XShapeCombineRectangles(dpy, xwin, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
	} else if (region.isEmpty()) {
		XShapeCombineMask(dpy, xwin, ShapeInput, 0, 0, 0, ShapeSet);
	} else {
		std::vector<XRectangle> rects;
		rects.reserve(static_cast<size_t>(region.rectCount()));
		for (const QRect &r : region) {
			XRectangle xr;
			xr.x = static_cast<short>(std::lround(r.x() * dpr));
			xr.y = static_cast<short>(std::lround(r.y() * dpr));
			xr.width = static_cast<unsigned short>(std::max<long>(1, std::lround(r.width() * dpr)));
			xr.height = static_cast<unsigned short>(std::max<long>(1, std::lround(r.height() * dpr)));
			rects.push_back(xr);
		}
		XShapeCombineRectangles(dpy, xwin, ShapeInput, 0, 0, rects.data(),
					static_cast<int>(rects.size()), ShapeSet, Unsorted);
	}
	XFlush(dpy);
}

void ApplyX11TopmostHints(QWidget *widget, bool clickThrough)
{
	if (!widget || !WaylandLayerHelper::IsX11Platform())
		return;
	auto *x11App = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
	if (!x11App)
		return;
	Display *dpy = x11App->display();
	if (!dpy)
		return;

	Window xwin = static_cast<Window>(widget->winId());
	if (!xwin)
		return;

	Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom wmAbove = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
	Atom wmSkipTaskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	Atom wmSkipPager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
	Atom states[3] = {wmAbove, wmSkipTaskbar, wmSkipPager};
	XChangeProperty(dpy, xwin, wmState, XA_ATOM, 32, PropModeReplace,
			reinterpret_cast<unsigned char *>(states), 3);

	if (clickThrough) {
		Atom wmType = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
		Atom wmNotification = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
		XChangeProperty(dpy, xwin, wmType, XA_ATOM, 32, PropModeReplace,
				reinterpret_cast<unsigned char *>(&wmNotification), 1);
	}
	XFlush(dpy);
}

} // namespace

bool WaylandLayerHelper::IsWaylandPlatform()
{
	return qGuiApp && qGuiApp->platformName().contains(QLatin1String("wayland"), Qt::CaseInsensitive);
}

bool WaylandLayerHelper::IsX11Platform()
{
	return qGuiApp && qGuiApp->platformName().contains(QLatin1String("xcb"), Qt::CaseInsensitive);
}

void WaylandLayerHelper::ConfigureWidget(QWidget *widget, const LayerSurfaceSpec &spec)
{
	if (!widget)
		return;

	if (!widget->windowHandle()) {
		Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
					Qt::NoDropShadowWindowHint;
		if (spec.keyboardMode == KeyboardMode::None && spec.clickThrough)
			flags |= Qt::WindowDoesNotAcceptFocus;
		if (spec.clickThrough)
			flags |= Qt::WindowTransparentForInput;
		if (IsX11Platform())
			flags |= Qt::Tool | Qt::X11BypassWindowManagerHint;

		widget->setWindowFlags(flags);
		widget->setAttribute(Qt::WA_TranslucentBackground, true);
		widget->setAttribute(Qt::WA_ShowWithoutActivating, spec.keyboardMode == KeyboardMode::None);
		widget->setAttribute(Qt::WA_NoSystemBackground, true);
	}

	if (spec.targetScreen)
		widget->setScreen(spec.targetScreen);

	if (!widget->windowHandle() || !widget->windowHandle()->handle())
		widget->winId();

	if (QWindow *qwin = widget->windowHandle()) {
		if (spec.targetScreen)
			qwin->setScreen(spec.targetScreen);
		WindowEntry &entry = WindowRegistry()[qwin];
		entry.spec = spec;

		qwin->removeEventFilter(GetGlobalEventFilter());
		qwin->installEventFilter(GetGlobalEventFilter());
		LayerSurfaceEventFilter::AttachShellIntegration(qwin);

		if (entry.surface)
			entry.surface->UpdateSpec(spec);
		else if (IsWaylandPlatform())
			ApplyWaylandToplevelInputRegion(widget, spec.inputRegion, spec.clickThrough);
	}

	if (IsX11Platform()) {
		ApplyX11TopmostHints(widget, spec.clickThrough);
		ApplyX11InputShape(widget, spec.inputRegion, spec.clickThrough);
	}
}

void WaylandLayerHelper::UpdateWidgetSpec(QWidget *widget, const LayerSurfaceSpec &spec)
{
	if (!widget)
		return;
	QWindow *qwin = widget->windowHandle();
	if (!qwin) {
		ConfigureWidget(widget, spec);
		return;
	}

	WindowEntry &entry = WindowRegistry()[qwin];
	entry.spec = spec;
	if (spec.targetScreen)
		qwin->setScreen(spec.targetScreen);
	if (entry.surface)
		entry.surface->UpdateSpec(spec);
	else if (IsWaylandPlatform())
		ApplyWaylandToplevelInputRegion(widget, spec.inputRegion, spec.clickThrough);

	if (IsX11Platform()) {
		ApplyX11TopmostHints(widget, spec.clickThrough);
		ApplyX11InputShape(widget, spec.inputRegion, spec.clickThrough);
	}
}

void WaylandLayerHelper::SetInputRegion(QWidget *widget, const QRegion &region, bool clickThrough)
{
	if (!widget)
		return;
	if (QWindow *qwin = widget->windowHandle()) {
		if (WindowRegistry().contains(qwin)) {
			WindowEntry &entry = WindowRegistry()[qwin];
			entry.spec.clickThrough = clickThrough;
			entry.spec.inputRegion = region;
			if (entry.surface)
				entry.surface->UpdateSpec(entry.spec);
			else if (IsWaylandPlatform())
				ApplyWaylandToplevelInputRegion(widget, region, clickThrough);
		}
	}
	if (IsX11Platform())
		ApplyX11InputShape(widget, region, clickThrough);
}

void WaylandLayerHelper::SetKeyboardMode(QWidget *widget, KeyboardMode mode)
{
	if (!widget)
		return;
	widget->setAttribute(Qt::WA_ShowWithoutActivating, mode == KeyboardMode::None);
	if (QWindow *qwin = widget->windowHandle()) {
		if (WindowRegistry().contains(qwin)) {
			WindowEntry &entry = WindowRegistry()[qwin];
			if (entry.spec.keyboardMode != mode) {
				entry.spec.keyboardMode = mode;
			}
		}
		std::lock_guard<std::mutex> lock(g_surfaceMutex);
		auto itSurf = g_activeSurfaces.find(qwin);
		if (itSurf != g_activeSurfaces.end() && itSurf->second)
			itSurf->second->updateKeyboardMode(mode);
	}
}

void WaylandLayerHelper::ShutdownGlobal()
{
	{
		std::lock_guard<std::mutex> lock(g_surfaceMutex);
		g_activeSurfaces.clear();
	}
	WindowRegistry().clear();
	GetGlobalShellIntegration()->Reset();
}

} // namespace overlay_win
