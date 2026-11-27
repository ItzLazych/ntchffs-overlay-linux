#include "overlay-smart-replay.h"

#include <QMetaObject>
#include <QTimer>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include "overlay-trimmer.h"
#include "overlay-window-c.h"
#include "plugin-support.h"

namespace {

bool g_enabled = false;
int g_mode = 0; // 0 = timestamp trim, 1 = legacy
bool g_clip_ignore_trim = false;

std::mutex g_sr_mutex;
std::deque<std::pair<uint64_t, uint64_t>> g_save_requests; // (seq, request_ns)
uint64_t g_next_seq = 1;
uint64_t g_prev_save_req_ns = 0;

uint64_t NowNs()
{
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

int GetReplayBufferMaxSecs()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 0;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	const char *sec = (mode && std::strcmp(mode, "Advanced") == 0) ? "AdvOut" : "SimpleOutput";
	return static_cast<int>(config_get_int(cfg, sec, "RecRBTime"));
}

void ScheduleLegacyRestart(int attempt)
{
	if (!g_enabled || g_mode != 1) {
		obs_log(LOG_INFO, "Smart replay: start cancelled (disabled or non-legacy mode)");
		return;
	}
	if (obs_frontend_replay_buffer_active()) {
		obs_log(LOG_INFO, "Smart replay: replay buffer already active");
		return;
	}
	if (attempt > 10) {
		obs_log(LOG_WARNING, "Smart replay: failed to restart replay buffer after %d attempts", attempt - 1);
		return;
	}
	obs_log(LOG_INFO, "Smart replay: starting replay buffer (attempt %d/%d)", attempt, 10);
	obs_frontend_replay_buffer_start();
	QTimer::singleShot(300, [attempt]() {
		if (obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Smart replay: replay buffer restart confirmed");
		} else {
			obs_log(LOG_INFO, "Smart replay: replay buffer did not start, retrying");
			ScheduleLegacyRestart(attempt + 1);
		}
	});
}

void TrimReplayOverlapAsync(uint64_t seq, const std::string &path, double keepTargetSec)
{
	std::thread([seq, path, keepTargetSec]() {
		auto t0 = std::chrono::steady_clock::now();
		obs_log(LOG_INFO, "[SmartReplay] trim worker start seq=%llu path=%s keep_target=%.3f s",
			static_cast<unsigned long long>(seq), path.c_str(), keepTargetSec);

		std::string tmpPath;
		PlaybackProbeResult preProbe;
		double plannedTrimStart = 0.0;
		std::string err;

		if (!OverlayTrimmer::TrimKeepLastSeconds(path, keepTargetSec, 0.25, tmpPath, &preProbe,
							 &plannedTrimStart, err)) {
			obs_log(LOG_WARNING, "[SmartReplay] trim worker failed seq=%llu path=%s error=%s",
				static_cast<unsigned long long>(seq), path.c_str(), err.c_str());
			return;
		}
		if (tmpPath.empty()) {
			obs_log(LOG_INFO,
				"[SmartReplay] trim worker skip seq=%llu path=%s reason=keep_ge_file keep=%.3f file_span=%.3f",
				static_cast<unsigned long long>(seq), path.c_str(), keepTargetSec,
				preProbe.spanSec);
			return;
		}

		overlay_window_release_file_if_open(path.c_str());
		std::error_code ec;
		std::filesystem::rename(tmpPath, path, ec);
		if (ec) {
			std::filesystem::remove(path, ec);
			if (ec) {
				obs_log(LOG_ERROR, "Smart replay trim: failed to delete original file");
				std::filesystem::remove(tmpPath, ec);
				return;
			}
			std::filesystem::rename(tmpPath, path, ec);
			if (ec) {
				obs_log(LOG_ERROR, "Smart replay trim: failed to move trimmed file into place");
				std::filesystem::remove(tmpPath, ec);
				return;
			}
		}

		PlaybackProbeResult postProbe;
		OverlayTrimmer::ProbePlaybackRange(path, postProbe, false);
		double workerSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		obs_log(LOG_INFO,
			"[SmartReplay] trim worker done seq=%llu path=%s keep_target=%.3f actual=%.3f delta=%+.3f removed_planned=%.3f worker_time=%.3f s",
			static_cast<unsigned long long>(seq), path.c_str(), keepTargetSec, postProbe.spanSec,
			postProbe.spanSec - keepTargetSec, plannedTrimStart, workerSec);
	}).detach();
}

} // namespace

extern "C" {

void overlay_smart_replay_set_enabled(bool enabled)
{
	g_enabled = enabled;
	overlay_smart_replay_reset_timestamps();
	obs_log(LOG_INFO, "Smart replay %s", enabled ? "enabled" : "disabled");
}

bool overlay_smart_replay_is_enabled(void)
{
	return g_enabled;
}

void overlay_smart_replay_set_mode(int mode)
{
	g_mode = std::clamp(mode, 0, 1);
	overlay_smart_replay_reset_timestamps();
	obs_log(LOG_INFO, "Smart replay mode: %s", g_mode == 0 ? "timestamp trim" : "legacy");
}

int overlay_smart_replay_get_mode(void)
{
	return g_mode;
}

void overlay_smart_replay_set_clip_ignore_trim(bool ignore)
{
	g_clip_ignore_trim = ignore;
	obs_log(LOG_INFO, "[SmartReplay] clip ignore trim=%s", ignore ? "yes" : "no");
}

bool overlay_smart_replay_get_clip_ignore_trim(void)
{
	return g_clip_ignore_trim;
}

void overlay_smart_replay_reset_timestamps(void)
{
	std::lock_guard<std::mutex> lock(g_sr_mutex);
	g_save_requests.clear();
	g_prev_save_req_ns = 0;
	obs_log(LOG_INFO, "[SmartReplay] timestamp state reset");
}

bool overlay_smart_replay_note_save_request(void)
{
	if (!g_enabled)
		return false;

	uint64_t now = NowNs();
	uint64_t seq = 0;
	uint64_t prev = 0;
	size_t depth = 0;
	{
		std::lock_guard<std::mutex> lock(g_sr_mutex);
		seq = g_next_seq++;
		prev = g_prev_save_req_ns;
		g_save_requests.push_back({seq, now});
		depth = g_save_requests.size();
	}

	double elapsedPrev = prev > 0 ? static_cast<double>(now - prev) / 1e9 : 0.0;
	int bufSec = GetReplayBufferMaxSecs();
	obs_log(LOG_INFO,
		"[SmartReplay] save requested seq=%llu request_ns=%llu prev_request_ns=%llu elapsed_since_prev=%.3f s queue_depth=%zu configured_buffer_sec=%d replay_active=%s",
		static_cast<unsigned long long>(seq), static_cast<unsigned long long>(now),
		static_cast<unsigned long long>(prev), elapsedPrev, depth, bufSec,
		obs_frontend_replay_buffer_active() ? "yes" : "no");
	return true;
}

void overlay_smart_replay_on_save_completed(const char *path, bool is_timed_clip)
{
	if (!g_enabled)
		return;

	uint64_t completeNs = NowNs();
	if (g_mode == 1) {
		if (is_timed_clip && g_clip_ignore_trim)
			return;
		if (obs_frontend_replay_buffer_active()) {
			obs_log(LOG_INFO, "Smart replay: stopping replay buffer for reset after save");
			obs_frontend_replay_buffer_stop();
			QTimer::singleShot(350, []() { ScheduleLegacyRestart(1); });
		}
		return;
	}

	uint64_t seq = 0;
	uint64_t reqNs = 0;
	uint64_t prevNs = 0;
	size_t depth = 0;
	{
		std::lock_guard<std::mutex> lock(g_sr_mutex);
		if (!g_save_requests.empty()) {
			seq = g_save_requests.front().first;
			reqNs = g_save_requests.front().second;
			g_save_requests.pop_front();
		}
		prevNs = g_prev_save_req_ns;
		if (reqNs > 0)
			g_prev_save_req_ns = reqNs;
		else
			g_prev_save_req_ns = completeNs;
		depth = g_save_requests.size();
	}

	if (is_timed_clip && g_clip_ignore_trim) {
		obs_log(LOG_INFO, "[SmartReplay] clip save acknowledged seq=%llu path=%s (overlap trim skipped)",
			static_cast<unsigned long long>(seq), path ? path : "(null)");
		return;
	}

	if (!path || !*path) {
		obs_log(LOG_WARNING, "[SmartReplay] save completed without a valid file path");
		return;
	}

	if (prevNs == 0) {
		obs_log(LOG_INFO, "[SmartReplay] trim skipped seq=%llu reason=first_save path=%s",
			static_cast<unsigned long long>(seq), path);
		return;
	}

	double elapsedSincePrev = static_cast<double>((reqNs > 0 ? reqNs : completeNs) - prevNs) / 1e9;
	int bufSec = GetReplayBufferMaxSecs();
	if (elapsedSincePrev < 1.0) {
		obs_log(LOG_INFO,
			"[SmartReplay] trim skipped seq=%llu reason=saves_too_close elapsed=%.3f s path=%s",
			static_cast<unsigned long long>(seq), elapsedSincePrev, path);
		return;
	}
	if (bufSec > 0 && elapsedSincePrev >= static_cast<double>(bufSec))
		return;

	obs_log(LOG_INFO,
		"[SmartReplay] trim scheduled seq=%llu path=%s keep_target=%.3f s (configured_buffer_sec=%d depth=%zu)",
		static_cast<unsigned long long>(seq), path, elapsedSincePrev, bufSec, depth);
	TrimReplayOverlapAsync(seq, path, elapsedSincePrev);
}

} // extern "C"
