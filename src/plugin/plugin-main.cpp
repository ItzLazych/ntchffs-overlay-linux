#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>

#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/input.h>
#include <memory>
#include <string>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "overlay-clip-save.h"
#include "overlay-indicators-c.h"
#include "overlay-settings-storage.h"
#include "overlay-smart-replay.h"
#include "overlay-toasts-c.h"
#include "overlay-window-c.h"
#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("ntchff")
OBS_MODULE_USE_DEFAULT_LOCALE("ntchffs-overlay", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "ntchff's Overlay";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Interactive overlay HUD, replay gallery, trimmer, indicators and toast notifications for OBS Studio (Linux Wayland/X11 port)";
}

namespace {

obs_hotkey_id g_toggle_hotkey_id = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id g_save_replay_hotkey_id = OBS_INVALID_HOTKEY_ID;
obs_data_t *g_preloaded_hotkey_data = nullptr;
bool g_initialized_ui = false;
bool g_exit_snapshot_done = false;

std::string FormatHotkeyArrayLabel(obs_data_array_t *arr)
{
	if (!arr || obs_data_array_count(arr) == 0)
		return {};
	obs_data_t *obj = obs_data_array_item(arr, 0);
	if (!obj)
		return {};

	std::string label;
	if (obs_data_get_bool(obj, "control"))
		label += "Ctrl+";
	if (obs_data_get_bool(obj, "alt"))
		label += "Alt+";
	if (obs_data_get_bool(obj, "shift"))
		label += "Shift+";

	const char *key = obs_data_get_string(obj, "key");
	if (key && *key) {
		std::string k(key);
		constexpr const char *kPrefix = "OBS_KEY_";
		if (k.rfind(kPrefix, 0) == 0)
			k = k.substr(std::strlen(kPrefix));
		if (!k.empty() && k != "NONE")
			label += k;
	}
	if (!label.empty() && label.back() == '+')
		label.pop_back();
	obs_data_release(obj);
	return label;
}

std::string FormatToggleOverlayHotkeyLabel()
{
	std::string res;
	if (g_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_toggle_hotkey_id);
		if (arr) {
			res = FormatHotkeyArrayLabel(arr);
			obs_data_array_release(arr);
		}
	}
	return res;
}

void RememberReplayBufferSnapshot(bool exiting)
{
	obs_data_t *s = overlay_settings_get();
	if (!s || !obs_data_get_bool(s, "remember_replay_buffer"))
		return;
	bool active = obs_frontend_replay_buffer_active();
	obs_data_set_bool(s, "replay_buffer_was_active", active);
	if (exiting) {
		obs_log(LOG_INFO, "Remember Replay Buffer: exit snapshot active=%s", active ? "yes" : "no");
		overlay_settings_save();
	}
}

void ToastActionCallback(int action_kind, const char *file_path, void *)
{
	if (action_kind == 0) {
		QTimer::singleShot(0, [p = std::string(file_path ? file_path : "")]() {
			overlay_window_open_file_in_gallery(p.c_str());
		});
	} else if (action_kind == 1 && file_path) {
		overlay_window_on_file_deleted(file_path);
	}
}

uint64_t SteadyNowMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					     .count());
}

std::atomic<uint64_t> g_lastToggleTriggerMs{0};
std::atomic<uint64_t> g_lastSaveTriggerMs{0};

bool TryClaimTrigger(std::atomic<uint64_t> &lastMs, uint64_t cooldownMs = 150)
{
	uint64_t now = SteadyNowMs();
	uint64_t prev = lastMs.load(std::memory_order_relaxed);
	while (true) {
		if (prev != 0 && now - prev < cooldownMs)
			return false;
		if (lastMs.compare_exchange_weak(prev, now, std::memory_order_acq_rel))
			return true;
	}
}

void TriggerToggleOverlay(const char *source = "hotkey")
{
	if (!TryClaimTrigger(g_lastToggleTriggerMs, 150))
		return;
	std::string src(source ? source : "hotkey");
	auto runOnGui = [src]() {
		overlay_window_toggle();
		obs_log(LOG_INFO, "Overlay toggled via %s (visible=%s)", src.c_str(),
			overlay_window_is_visible() ? "yes" : "no");
	};
	if (QCoreApplication *app = QCoreApplication::instance()) {
		if (QThread::currentThread() == app->thread())
			runOnGui();
		else
			QMetaObject::invokeMethod(app, runOnGui, Qt::QueuedConnection);
	}
}

void TriggerSaveReplay(const char *source = "hotkey")
{
	if (!TryClaimTrigger(g_lastSaveTriggerMs, 150))
		return;
	std::string src(source ? source : "hotkey");
	auto runOnGui = [src]() {
		if (!obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Save replay (%s): replay buffer not active", src.c_str());
			return;
		}
		if (overlay_clip_save_is_pending()) {
			obs_log(LOG_INFO, "Save replay (%s): save already in progress", src.c_str());
			return;
		}
		obs_log(LOG_INFO, "Save replay (%s): saving replay buffer", src.c_str());
		overlay_smart_replay_note_save_request();
		obs_frontend_replay_buffer_save();
	};
	if (QCoreApplication *app = QCoreApplication::instance()) {
		if (QThread::currentThread() == app->thread())
			runOnGui();
		else
			QMetaObject::invokeMethod(app, runOnGui, Qt::QueuedConnection);
	}
}

struct ParsedHotkeyCombo {
	bool ctrl = false;
	bool alt = false;
	bool shift = false;
	bool meta = false;
	std::string obsKey;
	int qtKey = 0;
	int evdevKey = 0;
	const char *x11KeySymName = nullptr;
	std::string niriXkbKey;

	// Edge state per polling backend
	bool x11WasDown = false;
	bool evdevWasDown = false;
};

struct KeyMappingEntry {
	const char *obsName;
	int qtKey;
	const char *x11Name;
	int evdevKey;
	const char *niriXkb;
};

const KeyMappingEntry *LookupObsKey(const std::string &obsKey)
{
	static const KeyMappingEntry kMap[] = {
		{"OBS_KEY_A", Qt::Key_A, "a", KEY_A, "A"},
		{"OBS_KEY_B", Qt::Key_B, "b", KEY_B, "B"},
		{"OBS_KEY_C", Qt::Key_C, "c", KEY_C, "C"},
		{"OBS_KEY_D", Qt::Key_D, "d", KEY_D, "D"},
		{"OBS_KEY_E", Qt::Key_E, "e", KEY_E, "E"},
		{"OBS_KEY_F", Qt::Key_F, "f", KEY_F, "F"},
		{"OBS_KEY_G", Qt::Key_G, "g", KEY_G, "G"},
		{"OBS_KEY_H", Qt::Key_H, "h", KEY_H, "H"},
		{"OBS_KEY_I", Qt::Key_I, "i", KEY_I, "I"},
		{"OBS_KEY_J", Qt::Key_J, "j", KEY_J, "J"},
		{"OBS_KEY_K", Qt::Key_K, "k", KEY_K, "K"},
		{"OBS_KEY_L", Qt::Key_L, "l", KEY_L, "L"},
		{"OBS_KEY_M", Qt::Key_M, "m", KEY_M, "M"},
		{"OBS_KEY_N", Qt::Key_N, "n", KEY_N, "N"},
		{"OBS_KEY_O", Qt::Key_O, "o", KEY_O, "O"},
		{"OBS_KEY_P", Qt::Key_P, "p", KEY_P, "P"},
		{"OBS_KEY_Q", Qt::Key_Q, "q", KEY_Q, "Q"},
		{"OBS_KEY_R", Qt::Key_R, "r", KEY_R, "R"},
		{"OBS_KEY_S", Qt::Key_S, "s", KEY_S, "S"},
		{"OBS_KEY_T", Qt::Key_T, "t", KEY_T, "T"},
		{"OBS_KEY_U", Qt::Key_U, "u", KEY_U, "U"},
		{"OBS_KEY_V", Qt::Key_V, "v", KEY_V, "V"},
		{"OBS_KEY_W", Qt::Key_W, "w", KEY_W, "W"},
		{"OBS_KEY_X", Qt::Key_X, "x", KEY_X, "X"},
		{"OBS_KEY_Y", Qt::Key_Y, "y", KEY_Y, "Y"},
		{"OBS_KEY_Z", Qt::Key_Z, "z", KEY_Z, "Z"},
		{"OBS_KEY_0", Qt::Key_0, "0", KEY_0, "0"},
		{"OBS_KEY_1", Qt::Key_1, "1", KEY_1, "1"},
		{"OBS_KEY_2", Qt::Key_2, "2", KEY_2, "2"},
		{"OBS_KEY_3", Qt::Key_3, "3", KEY_3, "3"},
		{"OBS_KEY_4", Qt::Key_4, "4", KEY_4, "4"},
		{"OBS_KEY_5", Qt::Key_5, "5", KEY_5, "5"},
		{"OBS_KEY_6", Qt::Key_6, "6", KEY_6, "6"},
		{"OBS_KEY_7", Qt::Key_7, "7", KEY_7, "7"},
		{"OBS_KEY_8", Qt::Key_8, "8", KEY_8, "8"},
		{"OBS_KEY_9", Qt::Key_9, "9", KEY_9, "9"},
		{"OBS_KEY_F1", Qt::Key_F1, "F1", KEY_F1, "F1"},
		{"OBS_KEY_F2", Qt::Key_F2, "F2", KEY_F2, "F2"},
		{"OBS_KEY_F3", Qt::Key_F3, "F3", KEY_F3, "F3"},
		{"OBS_KEY_F4", Qt::Key_F4, "F4", KEY_F4, "F4"},
		{"OBS_KEY_F5", Qt::Key_F5, "F5", KEY_F5, "F5"},
		{"OBS_KEY_F6", Qt::Key_F6, "F6", KEY_F6, "F6"},
		{"OBS_KEY_F7", Qt::Key_F7, "F7", KEY_F7, "F7"},
		{"OBS_KEY_F8", Qt::Key_F8, "F8", KEY_F8, "F8"},
		{"OBS_KEY_F9", Qt::Key_F9, "F9", KEY_F9, "F9"},
		{"OBS_KEY_F10", Qt::Key_F10, "F10", KEY_F10, "F10"},
		{"OBS_KEY_F11", Qt::Key_F11, "F11", KEY_F11, "F11"},
		{"OBS_KEY_F12", Qt::Key_F12, "F12", KEY_F12, "F12"},
		{"OBS_KEY_SPACE", Qt::Key_Space, "space", KEY_SPACE, "Space"},
		{"OBS_KEY_RETURN", Qt::Key_Return, "Return", KEY_ENTER, "Return"},
		{"OBS_KEY_TAB", Qt::Key_Tab, "Tab", KEY_TAB, "Tab"},
		{"OBS_KEY_ESCAPE", Qt::Key_Escape, "Escape", KEY_ESC, "Escape"},
		{"OBS_KEY_BACKSPACE", Qt::Key_Backspace, "BackSpace", KEY_BACKSPACE, "BackSpace"},
		{"OBS_KEY_INSERT", Qt::Key_Insert, "Insert", KEY_INSERT, "Insert"},
		{"OBS_KEY_DELETE", Qt::Key_Delete, "Delete", KEY_DELETE, "Delete"},
		{"OBS_KEY_HOME", Qt::Key_Home, "Home", KEY_HOME, "Home"},
		{"OBS_KEY_END", Qt::Key_End, "End", KEY_END, "End"},
		{"OBS_KEY_PAGEUP", Qt::Key_PageUp, "Page_Up", KEY_PAGEUP, "Page_Up"},
		{"OBS_KEY_PAGEDOWN", Qt::Key_PageDown, "Page_Down", KEY_PAGEDOWN, "Page_Down"},
		{"OBS_KEY_LEFT", Qt::Key_Left, "Left", KEY_LEFT, "Left"},
		{"OBS_KEY_RIGHT", Qt::Key_Right, "Right", KEY_RIGHT, "Right"},
		{"OBS_KEY_UP", Qt::Key_Up, "Up", KEY_UP, "Up"},
		{"OBS_KEY_DOWN", Qt::Key_Down, "Down", KEY_DOWN, "Down"},
		{"OBS_KEY_MINUS", Qt::Key_Minus, "minus", KEY_MINUS, "minus"},
		{"OBS_KEY_EQUAL", Qt::Key_Equal, "equal", KEY_EQUAL, "equal"},
		{"OBS_KEY_BRACKETLEFT", Qt::Key_BracketLeft, "bracketleft", KEY_LEFTBRACE, "bracketleft"},
		{"OBS_KEY_BRACKETRIGHT", Qt::Key_BracketRight, "bracketright", KEY_RIGHTBRACE, "bracketright"},
		{"OBS_KEY_BACKSLASH", Qt::Key_Backslash, "backslash", KEY_BACKSLASH, "backslash"},
		{"OBS_KEY_SEMICOLON", Qt::Key_Semicolon, "semicolon", KEY_SEMICOLON, "semicolon"},
		{"OBS_KEY_APOSTROPHE", Qt::Key_Apostrophe, "apostrophe", KEY_APOSTROPHE, "apostrophe"},
		{"OBS_KEY_COMMA", Qt::Key_Comma, "comma", KEY_COMMA, "comma"},
		{"OBS_KEY_PERIOD", Qt::Key_Period, "period", KEY_DOT, "period"},
		{"OBS_KEY_SLASH", Qt::Key_Slash, "slash", KEY_SLASH, "slash"},
		{"OBS_KEY_ASCIITILDE", Qt::Key_QuoteLeft, "grave", KEY_GRAVE, "grave"},
		{"OBS_KEY_QUOTELEFT", Qt::Key_QuoteLeft, "grave", KEY_GRAVE, "grave"},
		{"OBS_KEY_DEAD_GRAVE", Qt::Key_QuoteLeft, "grave", KEY_GRAVE, "grave"},
		{"OBS_KEY_PRINT", Qt::Key_Print, "Print", KEY_SYSRQ, "Print"},
		{"OBS_KEY_PAUSE", Qt::Key_Pause, "Pause", KEY_PAUSE, "Pause"},
		{"OBS_KEY_CAPSLOCK", Qt::Key_CapsLock, "Caps_Lock", KEY_CAPSLOCK, "Caps_Lock"},
	};
	for (const auto &e : kMap) {
		if (obsKey == e.obsName)
			return &e;
	}
	return nullptr;
}

std::vector<ParsedHotkeyCombo> ParseObsHotkeyBindings(obs_hotkey_id id)
{
	std::vector<ParsedHotkeyCombo> out;
	if (id == OBS_INVALID_HOTKEY_ID)
		return out;
	obs_data_array_t *arr = obs_hotkey_save(id);
	if (!arr)
		return out;

	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; ++i) {
		obs_data_t *obj = obs_data_array_item(arr, i);
		if (!obj)
			continue;
		ParsedHotkeyCombo c;
		c.ctrl = obs_data_get_bool(obj, "control");
		c.alt = obs_data_get_bool(obj, "alt");
		c.shift = obs_data_get_bool(obj, "shift");
		c.meta = obs_data_get_bool(obj, "command");
		if (const char *k = obs_data_get_string(obj, "key"))
			c.obsKey = k;
		obs_data_release(obj);

		if (c.obsKey == "OBS_KEY_NONE")
			c.obsKey.clear();

		if (!c.obsKey.empty()) {
			if (const auto *m = LookupObsKey(c.obsKey)) {
				c.qtKey = m->qtKey;
				c.x11KeySymName = m->x11Name;
				c.evdevKey = m->evdevKey;
				c.niriXkbKey = m->niriXkb;
			} else {
				continue; // Unknown unmappable key
			}
		} else if (!c.ctrl && !c.alt && !c.shift && !c.meta) {
			continue;
		}
		out.push_back(std::move(c));
	}
	obs_data_array_release(arr);
	return out;
}

std::vector<std::string> ComboToNiriBindKeys(const ParsedHotkeyCombo &c)
{
	std::vector<std::string> res;
	if (!c.niriXkbKey.empty()) {
		std::string b;
		if (c.ctrl)
			b += "Ctrl+";
		if (c.alt)
			b += "Alt+";
		if (c.shift)
			b += "Shift+";
		if (c.meta)
			b += "Super+";
		b += c.niriXkbKey;
		res.push_back(std::move(b));
		return res;
	}

	// Modifier-only combo: smithay updates XKB modifiers BEFORE matching niri keybinds,
	// so every active modifier in the chord must appear in the modifier prefix too.
	struct ModEntry {
		bool enabled;
		const char *prefix;
		const char *xkbL;
		const char *xkbR;
	};
	const ModEntry mods[4] = {
		{c.ctrl, "Ctrl+", "Control_L", "Control_R"},
		{c.alt, "Alt+", "Alt_L", "Alt_R"},
		{c.shift, "Shift+", "Shift_L", "Shift_R"},
		{c.meta, "Super+", "Super_L", "Super_R"},
	};

	std::string fullPrefix;
	for (int i = 0; i < 4; ++i) {
		if (mods[i].enabled)
			fullPrefix += mods[i].prefix;
	}
	for (int last = 0; last < 4; ++last) {
		if (!mods[last].enabled)
			continue;
		res.push_back(fullPrefix + mods[last].xkbL);
		res.push_back(fullPrefix + mods[last].xkbR);
	}
	return res;
}

class GlobalHotkeyMonitor : public QObject {
public:
	static GlobalHotkeyMonitor &Instance()
	{
		static GlobalHotkeyMonitor inst;
		return inst;
	}

	void Start()
	{
		if (started_)
			return;
		started_ = true;

		if (QCoreApplication *app = QCoreApplication::instance())
			app->installEventFilter(this);

		InitRuntimeCmdWatcher();
		InitX11();
		InitEvdev();
		ReloadBindingsAndSyncNiri();

		pollTimer_.setInterval(25);
		connect(&pollTimer_, &QTimer::timeout, this, &GlobalHotkeyMonitor::OnPollTick);
		pollTimer_.start();

		syncTimer_.setInterval(2000);
		connect(&syncTimer_, &QTimer::timeout, this, &GlobalHotkeyMonitor::ReloadBindingsAndSyncNiri);
		syncTimer_.start();
	}

	void Stop()
	{
		if (!started_)
			return;
		started_ = false;
		pollTimer_.stop();
		syncTimer_.stop();

		if (QCoreApplication *app = QCoreApplication::instance())
			app->removeEventFilter(this);

		if (cmdNotifier_) {
			delete cmdNotifier_;
			cmdNotifier_ = nullptr;
		}
		if (inotifyFd_ >= 0) {
			::close(inotifyFd_);
			inotifyFd_ = -1;
		}
		for (int fd : evdevFds_) {
			if (fd >= 0)
				::close(fd);
		}
		evdevFds_.clear();
		evdevKeyState_.fill(false);

		if (x11Display_ && fnXCloseDisplay_)
			fnXCloseDisplay_(x11Display_);
		x11Display_ = nullptr;
		if (x11Lib_) {
			dlclose(x11Lib_);
			x11Lib_ = nullptr;
		}
	}

	void ReloadBindingsAndSyncNiri()
	{
		auto nextToggle = ParseObsHotkeyBindings(g_toggle_hotkey_id);
		auto nextSave = ParseObsHotkeyBindings(g_save_replay_hotkey_id);

		auto preserveEdges = [](std::vector<ParsedHotkeyCombo> &next,
					const std::vector<ParsedHotkeyCombo> &prev) {
			for (auto &nc : next) {
				for (const auto &pc : prev) {
					if (nc.ctrl == pc.ctrl && nc.alt == pc.alt && nc.shift == pc.shift &&
					    nc.meta == pc.meta && nc.obsKey == pc.obsKey) {
						nc.x11WasDown = pc.x11WasDown;
						nc.evdevWasDown = pc.evdevWasDown;
						break;
					}
				}
			}
		};
		preserveEdges(nextToggle, toggleCombos_);
		preserveEdges(nextSave, saveCombos_);

		toggleCombos_ = std::move(nextToggle);
		saveCombos_ = std::move(nextSave);

		SyncNiriBindsFile();
	}

protected:
	bool eventFilter(QObject *, QEvent *ev) override
	{
		if (!started_)
			return false;
		if (ev->type() == QEvent::WindowDeactivate || ev->type() == QEvent::FocusOut ||
		    ev->type() == QEvent::ApplicationStateChange) {
			qtToggleWasDown_ = false;
			qtSaveWasDown_ = false;
			return false;
		}
		if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::KeyRelease)
			return false;

		auto *kev = static_cast<QKeyEvent *>(ev);
		if (kev->isAutoRepeat())
			return false;

		const int key = kev->key();
		const bool pressed = (ev->type() == QEvent::KeyPress);
		Qt::KeyboardModifiers mods = kev->modifiers();
		if (key == Qt::Key_Control) {
			if (pressed)
				mods |= Qt::ControlModifier;
			else
				mods &= ~Qt::ControlModifier;
		} else if (key == Qt::Key_Alt || key == Qt::Key_AltGr) {
			if (pressed)
				mods |= Qt::AltModifier;
			else
				mods &= ~Qt::AltModifier;
		} else if (key == Qt::Key_Shift) {
			if (pressed)
				mods |= Qt::ShiftModifier;
			else
				mods &= ~Qt::ShiftModifier;
		} else if (key == Qt::Key_Meta || key == Qt::Key_Super_L || key == Qt::Key_Super_R) {
			if (pressed)
				mods |= Qt::MetaModifier;
			else
				mods &= ~Qt::MetaModifier;
		}

		if (!pressed) {
			qtToggleWasDown_ = false;
			qtSaveWasDown_ = false;
			return false;
		}

		// Don't intercept if OBS settings modal/hotkey capture is open
		if (QApplication::activeModalWidget() != nullptr && !overlay_window_is_visible())
			return false;

		auto matchCombo = [&](const ParsedHotkeyCombo &c) -> bool {
			bool modMatch = (static_cast<bool>(mods & Qt::ControlModifier) == c.ctrl) &&
					(static_cast<bool>(mods & Qt::AltModifier) == c.alt) &&
					(static_cast<bool>(mods & Qt::ShiftModifier) == c.shift) &&
					(static_cast<bool>(mods & Qt::MetaModifier) == c.meta);
			if (!modMatch)
				return false;
			if (c.qtKey != 0)
				return key == c.qtKey;
			return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_AltGr ||
			       key == Qt::Key_Shift || key == Qt::Key_Meta || key == Qt::Key_Super_L ||
			       key == Qt::Key_Super_R;
		};

		for (const auto &c : toggleCombos_) {
			if (matchCombo(c)) {
				if (!qtToggleWasDown_) {
					qtToggleWasDown_ = true;
					TriggerToggleOverlay("qt-filter");
				}
				return overlay_window_is_visible();
			}
		}
		for (const auto &c : saveCombos_) {
			if (matchCombo(c)) {
				if (!qtSaveWasDown_) {
					qtSaveWasDown_ = true;
					TriggerSaveReplay("qt-filter");
				}
				return overlay_window_is_visible();
			}
		}
		return false;
	}

private:
	void InitRuntimeCmdWatcher()
	{
		const char *rtDir = std::getenv("XDG_RUNTIME_DIR");
		std::filesystem::path dir = (rtDir && *rtDir) ? std::filesystem::path(rtDir)
							     : std::filesystem::path("/tmp");
		cmdDir_ = dir.string();
		cmdFileName_ = "ntchffs-overlay.cmd";
		cmdFilePath_ = (dir / cmdFileName_).string();

		int fd = ::open(cmdFilePath_.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
		if (fd >= 0)
			::close(fd);
		cmdReadOffset_ = 0;

		inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (inotifyFd_ >= 0) {
			if (inotify_add_watch(inotifyFd_, cmdDir_.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) >= 0) {
				cmdNotifier_ = new QSocketNotifier(inotifyFd_, QSocketNotifier::Read, this);
				connect(cmdNotifier_, &QSocketNotifier::activated, this,
					&GlobalHotkeyMonitor::OnInotifyActivated);
			} else {
				::close(inotifyFd_);
				inotifyFd_ = -1;
			}
		}
	}

	void OnInotifyActivated()
	{
		if (inotifyFd_ < 0)
			return;
		char buf[4096];
		bool cmdTouched = false;
		while (true) {
			ssize_t len = ::read(inotifyFd_, buf, sizeof(buf));
			if (len <= 0)
				break;
			for (char *p = buf; p < buf + len;) {
				auto *ev = reinterpret_cast<const struct inotify_event *>(p);
				if (ev->len > 0 && std::strcmp(ev->name, cmdFileName_.c_str()) == 0)
					cmdTouched = true;
				p += sizeof(struct inotify_event) + ev->len;
			}
		}
		if (!cmdTouched)
			return;

		// Open read-only so closing never emits IN_CLOSE_WRITE, and read appended lines
		// from cmdReadOffset_ so concurrent writers appending with >> never race with ftruncate.
		int fd = ::open(cmdFilePath_.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return;
		struct stat st{};
		if (::fstat(fd, &st) == 0 && st.st_size < cmdReadOffset_)
			cmdReadOffset_ = 0;
		::lseek(fd, cmdReadOffset_, SEEK_SET);

		std::string cmds;
		char data[1024];
		while (true) {
			ssize_t n = ::read(fd, data, sizeof(data));
			if (n > 0)
				cmds.append(data, static_cast<size_t>(n));
			else if (n < 0 && errno == EINTR)
				continue;
			else
				break;
		}
		::close(fd);
		if (cmds.empty())
			return;
		cmdReadOffset_ += static_cast<off_t>(cmds.size());

		size_t pos = 0;
		while (pos < cmds.size()) {
			size_t nl = cmds.find('\n', pos);
			std::string line = (nl == std::string::npos) ? cmds.substr(pos)
								     : cmds.substr(pos, nl - pos);
			pos = (nl == std::string::npos) ? cmds.size() : (nl + 1);
			if (line == "toggle_overlay")
				TriggerToggleOverlay("niri-ipc");
			else if (line == "save_replay")
				TriggerSaveReplay("niri-ipc");
		}
	}

	void SyncNiriBindsFile()
	{
		if (!started_ || cmdFilePath_.empty())
			return;
		const char *niriSock = std::getenv("NIRI_SOCKET");
		const char *home = std::getenv("HOME");
		if (!niriSock || !*niriSock || !home || !*home)
			return;

		std::filesystem::path niriDir = std::filesystem::path(home) / ".config" / "niri";
		std::filesystem::path customKdl = niriDir / "custom.kdl";
		if (!std::filesystem::exists(customKdl))
			customKdl = niriDir / "config.kdl";
		if (!std::filesystem::exists(customKdl))
			return;

		std::filesystem::path bindsKdl = niriDir / "ntchffs-overlay-binds.kdl";
		std::filesystem::path tmpKdl = niriDir / "ntchffs-overlay-binds.kdl.tmp";

		std::string kdl = "// Auto-generated by ntchffs-overlay for global OBS hotkeys on niri\nbinds {\n";
		auto appendCombos = [&](const std::vector<ParsedHotkeyCombo> &combos, const char *cmdName) {
			std::vector<std::string> emitted;
			for (const auto &c : combos) {
				for (const std::string &bind : ComboToNiriBindKeys(c)) {
					if (std::find(emitted.begin(), emitted.end(), bind) != emitted.end())
						continue;
					emitted.push_back(bind);
					kdl += "    " + bind +
					       " repeat=false allow-inhibiting=false { spawn-sh \"printf '" +
					       cmdName + "\\\\n' >> '" + cmdFilePath_ + "'\"; }\n";
				}
			}
		};
		appendCombos(toggleCombos_, "toggle_overlay");
		appendCombos(saveCombos_, "save_replay");
		kdl += "}\n";

		if (lastSyncedKdl_ == kdl && customIncludeVerified_)
			return;

		std::string existingBinds;
		{
			std::ifstream in(bindsKdl, std::ios::binary);
			if (in)
				existingBinds.assign(std::istreambuf_iterator<char>(in),
						     std::istreambuf_iterator<char>());
		}

		bool changed = false;
		if (existingBinds != kdl) {
			{
				std::ofstream out(tmpKdl, std::ios::binary | std::ios::trunc);
				if (!out)
					return;
				out.write(kdl.data(), static_cast<std::streamsize>(kdl.size()));
			}
			int valRc = QProcess::execute(QStringLiteral("niri"),
						      {QStringLiteral("validate"), QStringLiteral("-c"),
						       QString::fromUtf8(tmpKdl.c_str())});
			if (valRc == 0 || valRc == -2) {
				std::error_code ec;
				std::filesystem::rename(tmpKdl, bindsKdl, ec);
				if (!ec) {
					changed = true;
					obs_log(LOG_INFO, "Updated niri hotkey binds: %s", bindsKdl.c_str());
				}
			} else {
				obs_log(LOG_WARNING,
					"Generated %s failed `niri validate` (rc=%d), keeping previous config",
					tmpKdl.c_str(), valRc);
				std::error_code ec;
				std::filesystem::remove(tmpKdl, ec);
				return;
			}
		}

		// Ensure include "ntchffs-overlay-binds.kdl" exists in custom.kdl
		std::string customContent;
		{
			std::ifstream in(customKdl, std::ios::binary);
			if (in)
				customContent.assign(std::istreambuf_iterator<char>(in),
						     std::istreambuf_iterator<char>());
		}
		if (customContent.find("ntchffs-overlay-binds.kdl") == std::string::npos) {
			if (!customContent.empty() && customContent.back() != '\n')
				customContent.push_back('\n');
			customContent += "\n// OBS Overlay global hotkeys (auto-managed)\n";
			customContent += "include \"ntchffs-overlay-binds.kdl\"\n";
			std::ofstream out(customKdl, std::ios::binary | std::ios::trunc);
			if (out) {
				out.write(customContent.data(), static_cast<std::streamsize>(customContent.size()));
				changed = true;
				obs_log(LOG_INFO, "Added include \"ntchffs-overlay-binds.kdl\" to %s",
					customKdl.c_str());
			}
		}

		lastSyncedKdl_ = kdl;
		customIncludeVerified_ = true;

		if (changed) {
			bool ok = QProcess::startDetached(
				QStringLiteral("niri"),
				{QStringLiteral("msg"), QStringLiteral("action"), QStringLiteral("load-config-file")});
			obs_log(LOG_INFO, "Requested niri config reload via load-config-file (dispatched=%s)",
				ok ? "yes" : "no");
		}
	}

	void InitX11()
	{
		const char *dispEnv = std::getenv("DISPLAY");
		if (!dispEnv || !*dispEnv)
			return;
		x11Lib_ = dlopen("libX11.so.6", RTLD_LAZY);
		if (!x11Lib_)
			return;
		fnXOpenDisplay_ = reinterpret_cast<FnXOpenDisplay>(dlsym(x11Lib_, "XOpenDisplay"));
		fnXCloseDisplay_ = reinterpret_cast<FnXCloseDisplay>(dlsym(x11Lib_, "XCloseDisplay"));
		fnXQueryKeymap_ = reinterpret_cast<FnXQueryKeymap>(dlsym(x11Lib_, "XQueryKeymap"));
		fnXKeysymToKeycode_ = reinterpret_cast<FnXKeysymToKeycode>(dlsym(x11Lib_, "XKeysymToKeycode"));
		fnXStringToKeysym_ = reinterpret_cast<FnXStringToKeysym>(dlsym(x11Lib_, "XStringToKeysym"));

		if (!fnXOpenDisplay_ || !fnXCloseDisplay_ || !fnXQueryKeymap_ || !fnXKeysymToKeycode_ ||
		    !fnXStringToKeysym_)
			return;

		x11Display_ = fnXOpenDisplay_(nullptr);
		if (x11Display_)
			obs_log(LOG_INFO, "GlobalHotkeyMonitor: X11/XWayland keymap watcher enabled");
	}

	void InitEvdev()
	{
		evdevKeyState_.fill(false);
		for (int i = 0; i < 32; ++i) {
			std::string path = "/dev/input/event" + std::to_string(i);
			int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0)
				continue;
			unsigned long keyBits[(KEY_MAX + 7) / 8 / sizeof(unsigned long) + 2]{};
			if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0) {
				auto testBit = [&](int bit) -> bool {
					return (keyBits[bit / (8 * sizeof(unsigned long))] >>
						(bit % (8 * sizeof(unsigned long)))) &
					       1UL;
				};
				if (testBit(KEY_LEFTCTRL) || testBit(KEY_A)) {
					evdevFds_.push_back(fd);
					continue;
				}
			}
			::close(fd);
		}
		if (!evdevFds_.empty())
			obs_log(LOG_INFO, "GlobalHotkeyMonitor: opened %zu evdev keyboard device(s)",
				evdevFds_.size());
	}

	bool IsX11KeyDown(const char keys[32], const char *symName) const
	{
		if (!x11Display_ || !symName || !*symName)
			return false;
		unsigned long ks = fnXStringToKeysym_(symName);
		if (!ks)
			return false;
		unsigned char kc = fnXKeysymToKeycode_(x11Display_, ks);
		if (!kc)
			return false;
		return (keys[kc >> 3] & (1 << (kc & 7))) != 0;
	}

	bool IsComboDownX11(const ParsedHotkeyCombo &c, const char keys[32]) const
	{
		bool ctrl = IsX11KeyDown(keys, "Control_L") || IsX11KeyDown(keys, "Control_R");
		bool alt = IsX11KeyDown(keys, "Alt_L") || IsX11KeyDown(keys, "Alt_R");
		bool shift = IsX11KeyDown(keys, "Shift_L") || IsX11KeyDown(keys, "Shift_R");
		bool meta = IsX11KeyDown(keys, "Super_L") || IsX11KeyDown(keys, "Super_R");
		if (ctrl != c.ctrl || alt != c.alt || shift != c.shift || meta != c.meta)
			return false;
		if (c.x11KeySymName)
			return IsX11KeyDown(keys, c.x11KeySymName);
		return c.ctrl || c.alt || c.shift || c.meta;
	}

	bool IsComboDownEvdev(const ParsedHotkeyCombo &c) const
	{
		bool ctrl = evdevKeyState_[KEY_LEFTCTRL] || evdevKeyState_[KEY_RIGHTCTRL];
		bool alt = evdevKeyState_[KEY_LEFTALT] || evdevKeyState_[KEY_RIGHTALT];
		bool shift = evdevKeyState_[KEY_LEFTSHIFT] || evdevKeyState_[KEY_RIGHTSHIFT];
		bool meta = evdevKeyState_[KEY_LEFTMETA] || evdevKeyState_[KEY_RIGHTMETA];
		if (ctrl != c.ctrl || alt != c.alt || shift != c.shift || meta != c.meta)
			return false;
		if (c.evdevKey > 0 && c.evdevKey < KEY_MAX)
			return evdevKeyState_[static_cast<size_t>(c.evdevKey)];
		return c.ctrl || c.alt || c.shift || c.meta;
	}

	void OnPollTick()
	{
		if (x11Display_ && fnXQueryKeymap_) {
			char keys[32]{};
			fnXQueryKeymap_(x11Display_, keys);
			for (auto &c : toggleCombos_) {
				bool down = IsComboDownX11(c, keys);
				if (down && !c.x11WasDown)
					TriggerToggleOverlay("x11");
				c.x11WasDown = down;
			}
			for (auto &c : saveCombos_) {
				bool down = IsComboDownX11(c, keys);
				if (down && !c.x11WasDown)
					TriggerSaveReplay("x11");
				c.x11WasDown = down;
			}
		}

		if (!evdevFds_.empty()) {
			struct input_event evs[64];
			bool anyEvent = false;
			for (int fd : evdevFds_) {
				while (true) {
					ssize_t rd = ::read(fd, evs, sizeof(evs));
					if (rd <= 0)
						break;
					anyEvent = true;
					size_t count = static_cast<size_t>(rd) / sizeof(struct input_event);
					for (size_t i = 0; i < count; ++i) {
						if (evs[i].type == EV_KEY && evs[i].code < KEY_MAX &&
						    evs[i].value != 2) {
							evdevKeyState_[evs[i].code] = (evs[i].value == 1);
						}
					}
				}
			}
			if (anyEvent) {
				for (auto &c : toggleCombos_) {
					bool down = IsComboDownEvdev(c);
					if (down && !c.evdevWasDown)
						TriggerToggleOverlay("evdev");
					c.evdevWasDown = down;
				}
				for (auto &c : saveCombos_) {
					bool down = IsComboDownEvdev(c);
					if (down && !c.evdevWasDown)
						TriggerSaveReplay("evdev");
					c.evdevWasDown = down;
				}
			}
		}
	}

	bool started_ = false;
	QTimer pollTimer_;
	QTimer syncTimer_;

	std::string cmdDir_;
	std::string cmdFileName_;
	std::string cmdFilePath_;
	off_t cmdReadOffset_ = 0;
	int inotifyFd_ = -1;
	QSocketNotifier *cmdNotifier_ = nullptr;

	void *x11Lib_ = nullptr;
	void *x11Display_ = nullptr;
	using FnXOpenDisplay = void *(*)(const char *);
	using FnXCloseDisplay = int (*)(void *);
	using FnXQueryKeymap = int (*)(void *, char[32]);
	using FnXKeysymToKeycode = unsigned char (*)(void *, unsigned long);
	using FnXStringToKeysym = unsigned long (*)(const char *);
	FnXOpenDisplay fnXOpenDisplay_ = nullptr;
	FnXCloseDisplay fnXCloseDisplay_ = nullptr;
	FnXQueryKeymap fnXQueryKeymap_ = nullptr;
	FnXKeysymToKeycode fnXKeysymToKeycode_ = nullptr;
	FnXStringToKeysym fnXStringToKeysym_ = nullptr;

	std::vector<int> evdevFds_;
	std::array<bool, KEY_MAX> evdevKeyState_{};

	bool qtToggleWasDown_ = false;
	bool qtSaveWasDown_ = false;
	std::string lastSyncedKdl_;
	bool customIncludeVerified_ = false;
	std::vector<ParsedHotkeyCombo> toggleCombos_;
	std::vector<ParsedHotkeyCombo> saveCombos_;
};

void ToggleOverlayHotkeyCallback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	TriggerToggleOverlay("obs-hotkey");
}

void SaveReplayHotkeyCallback(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	TriggerSaveReplay("obs-hotkey");
}

void SaveHotkeysCallback(obs_data_t *save_data, bool saving, void *)
{
	if (!saving || !save_data)
		return;
	obs_data_t *obj = obs_data_create();
	if (g_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_toggle_hotkey_id);
		obs_data_set_array(obj, "toggle_overlay", arr);
		obs_data_array_release(arr);
	}
	if (g_save_replay_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *arr = obs_hotkey_save(g_save_replay_hotkey_id);
		obs_data_set_array(obj, "save_replay", arr);
		obs_data_array_release(arr);
	}
	char key[64];
	std::snprintf(key, sizeof(key), "%s.hotkeys", PLUGIN_NAME);
	obs_data_set_obj(save_data, key, obj);
	obs_data_release(obj);
	obs_log(LOG_INFO, "Hotkeys saved to configuration with key: %s", key);
	GlobalHotkeyMonitor::Instance().ReloadBindingsAndSyncNiri();
}

void PreloadHotkeysCallback(obs_data_t *load_data, bool saving, void *)
{
	if (saving || !load_data)
		return;
	char key[64];
	std::snprintf(key, sizeof(key), "%s.hotkeys", PLUGIN_NAME);
	obs_data_t *obj = obs_data_get_obj(load_data, key);
	if (obj) {
		if (g_preloaded_hotkey_data)
			obs_data_release(g_preloaded_hotkey_data);
		g_preloaded_hotkey_data = obj;
		obs_log(LOG_INFO, "Hotkey data preloaded from configuration with key: %s", key);

		if (g_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
			obs_data_array_t *arr = obs_data_get_array(obj, "toggle_overlay");
			obs_hotkey_load(g_toggle_hotkey_id, arr);
			obs_data_array_release(arr);
		}
		if (g_save_replay_hotkey_id != OBS_INVALID_HOTKEY_ID) {
			obs_data_array_t *arr = obs_data_get_array(obj, "save_replay");
			obs_hotkey_load(g_save_replay_hotkey_id, arr);
			obs_data_array_release(arr);
		}
		GlobalHotkeyMonitor::Instance().ReloadBindingsAndSyncNiri();
	} else {
		obs_log(LOG_INFO, "No saved hotkey data found with key: %s", key);
	}
}

void HandleReplaySavedEvent()
{
	overlay_window_notify_replay_saving(false);

	char *replayPath = obs_frontend_get_last_replay();
	if (replayPath && *replayPath) {
		obs_log(LOG_INFO, "Replay saved: %s", replayPath);
		overlay_toasts_show_replay_saved(replayPath, false);
	} else {
		obs_log(LOG_WARNING, "Replay buffer saved event without a valid file path");
	}
	overlay_indicators_notify_replay_saved();
	if (!overlay_indicators_play_sound(4))
		obs_log(LOG_DEBUG, "Indicators: PlaySound skipped/empty for event %d", 4);

	bool timed = overlay_clip_save_on_replay_saved(replayPath);
	if (timed) {
		obs_log(LOG_INFO,
			"[ClipSave] replay_saved routed to timed-clip path (Smart Replay overlap trim skipped)");
	} else {
		obs_log(LOG_INFO,
			"[ClipSave] replay_saved routed to Smart Replay full save-completed path");
	}
	overlay_smart_replay_on_save_completed(replayPath, timed);

	if (replayPath)
		bfree(replayPath);
}

void InitializeUiOnObsReady()
{
	if (g_initialized_ui)
		return;
	g_initialized_ui = true;

	obs_log(LOG_INFO, "Attempting to create overlay window...");
	if (overlay_window_create()) {
		obs_log(LOG_INFO, "Overlay window created successfully");
		obs_log(LOG_INFO, "Overlay window created but not shown. Use hotkey to toggle visibility.");
	} else {
		obs_log(LOG_ERROR, "Failed to create overlay window - check Qt initialization");
	}

	obs_log(LOG_INFO, "Attempting to create overlay indicators...");
	if (overlay_indicators_create())
		obs_log(LOG_INFO, "Overlay indicators created successfully");
	else
		obs_log(LOG_WARNING, "Failed to create overlay indicators");

	obs_log(LOG_INFO, "Attempting to create overlay toasts...");
	if (overlay_toasts_create()) {
		obs_log(LOG_INFO, "Overlay toasts created successfully");
		overlay_toasts_set_action_callback(&ToastActionCallback, nullptr);
	} else {
		obs_log(LOG_WARNING, "Failed to create overlay toasts");
	}

	overlay_window_reload_settings();
	GlobalHotkeyMonitor::Instance().Start();

	obs_frontend_add_tools_menu_item(obs_module_text("OverlaySettings"),
					 [](void *) { overlay_window_open_settings(); }, nullptr);

	obs_data_t *s = overlay_settings_get();
	if (s && obs_data_get_bool(s, "remember_replay_buffer")) {
		bool wasActive = obs_data_get_bool(s, "replay_buffer_was_active");
		if (obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Remember Replay Buffer: already active, skip start");
		} else if (wasActive) {
			obs_log(LOG_INFO, "Remember Replay Buffer: starting once to restore last state");
			obs_frontend_replay_buffer_start();
		} else {
			obs_log(LOG_INFO, "Remember Replay Buffer: last state was inactive, skip start");
		}
	}

	QTimer::singleShot(1500, []() {
		obs_data_t *st = overlay_settings_get();
		bool toastsOn = !st || !obs_data_has_user_value(st, "toasts_enabled") ||
				obs_data_get_bool(st, "toasts_enabled");
		bool hintOn = !st || !obs_data_has_user_value(st, "toast_open_hint") ||
			      obs_data_get_bool(st, "toast_open_hint");
		if (toastsOn && hintOn) {
			std::string hk = FormatToggleOverlayHotkeyLabel();
			overlay_toasts_show_open_hint(hk.c_str());
			obs_log(LOG_INFO, "Overlay open-hint toast shown (hotkey: %s)",
				hk.empty() ? "unset" : hk.c_str());
		}
	});
}

void FrontendEventCallback(enum obs_frontend_event event, void *)
{
	obs_log(LOG_DEBUG, "Frontend event received: %d", static_cast<int>(event));
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		obs_log(LOG_INFO, "OBS finished loading, creating overlay window...");
		QTimer::singleShot(0, []() { InitializeUiOnObsReady(); });
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		overlay_indicators_update_state(true, obs_frontend_replay_buffer_active(), false);
		overlay_indicators_play_sound(0);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		overlay_indicators_update_state(false, obs_frontend_replay_buffer_active(), false);
		overlay_indicators_play_sound(1);
		char *rec = obs_frontend_get_last_recording();
		if (rec && *rec) {
			obs_log(LOG_INFO, "Recording saved: %s", rec);
			overlay_toasts_show_replay_saved(rec, true);
		} else {
			obs_log(LOG_WARNING, "Recording stopped without a valid file path");
		}
		if (rec)
			bfree(rec);
		break;
	}
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		overlay_smart_replay_reset_timestamps();
		overlay_indicators_update_state(obs_frontend_recording_active(), true, false);
		overlay_indicators_play_sound(2);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		overlay_smart_replay_reset_timestamps();
		overlay_window_notify_replay_saving(false);
		overlay_indicators_update_state(obs_frontend_recording_active(), false, false);
		overlay_indicators_play_sound(3);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVING:
		overlay_window_notify_replay_saving(true);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		HandleReplaySavedEvent();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (!g_exit_snapshot_done) {
			g_exit_snapshot_done = true;
			RememberReplayBufferSnapshot(true);
		}
		break;
	default:
		break;
	}
}

} // namespace

MODULE_EXPORT bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	obs_data_t *s = overlay_settings_load();
	if (s) {
		if (obs_data_has_user_value(s, "smart_replay"))
			overlay_smart_replay_set_enabled(obs_data_get_bool(s, "smart_replay"));
		if (obs_data_has_user_value(s, "smart_replay_mode"))
			overlay_smart_replay_set_mode(static_cast<int>(obs_data_get_int(s, "smart_replay_mode")));
		if (obs_data_has_user_value(s, "smart_replay_clip_ignore_trim"))
			overlay_smart_replay_set_clip_ignore_trim(
				obs_data_get_bool(s, "smart_replay_clip_ignore_trim"));
	}

	g_toggle_hotkey_id = obs_hotkey_register_frontend("obs-plugin-overlay.toggle_overlay",
							  obs_module_text("ToggleOverlay"),
							  &ToggleOverlayHotkeyCallback, nullptr);
	g_save_replay_hotkey_id = obs_hotkey_register_frontend("obs-plugin-overlay.save_replay",
							       obs_module_text("SaveReplay"),
							       &SaveReplayHotkeyCallback, nullptr);
	obs_log(LOG_INFO, "Hotkey registered");

	obs_frontend_add_save_callback(&SaveHotkeysCallback, nullptr);
	obs_frontend_add_preload_callback(&PreloadHotkeysCallback, nullptr);
	obs_log(LOG_INFO, "Registered hotkey save/preload callbacks");

	obs_frontend_add_event_callback(&FrontendEventCallback, nullptr);
	obs_log(LOG_INFO, "Registered frontend event callback for overlay creation");
	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	GlobalHotkeyMonitor::Instance().Stop();
	obs_frontend_remove_event_callback(&FrontendEventCallback, nullptr);
	obs_frontend_remove_save_callback(&SaveHotkeysCallback, nullptr);
	obs_frontend_remove_preload_callback(&PreloadHotkeysCallback, nullptr);

	if (g_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_toggle_hotkey_id);
		g_toggle_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_save_replay_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_save_replay_hotkey_id);
		g_save_replay_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_preloaded_hotkey_data) {
		obs_data_release(g_preloaded_hotkey_data);
		g_preloaded_hotkey_data = nullptr;
	}

	overlay_window_destroy();
	obs_log(LOG_INFO, "Overlay window destroyed");
	overlay_indicators_destroy();
	obs_log(LOG_INFO, "Overlay indicators destroyed");
	overlay_toasts_destroy();
	obs_log(LOG_INFO, "Overlay toasts destroyed");

	overlay_settings_save();
	overlay_settings_free();

	obs_log(LOG_INFO, "plugin unloaded");
}
