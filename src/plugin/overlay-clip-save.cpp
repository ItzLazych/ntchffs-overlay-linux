#include "overlay-clip-save.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "overlay-trimmer.h"
#include "overlay-window-c.h"
#include "plugin-support.h"

namespace {

std::atomic<int> g_pending_keep_seconds{0};
std::mutex g_trim_mutex;

const char *ReplayBufferConfigSection(config_t *cfg)
{
	if (!cfg)
		return "SimpleOutput";
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::strcmp(mode, "Advanced") == 0)
		return "AdvOut";
	return "SimpleOutput";
}

bool ReplaceFileWithTrimmed(const std::string &origPath, const std::string &trimmedPath)
{
	std::error_code ec;
	uint64_t origSz = std::filesystem::exists(origPath, ec)
				  ? static_cast<uint64_t>(std::filesystem::file_size(origPath, ec))
				  : 0;
	uint64_t trimSz = std::filesystem::exists(trimmedPath, ec)
				  ? static_cast<uint64_t>(std::filesystem::file_size(trimmedPath, ec))
				  : 0;
	obs_log(LOG_INFO, "[ClipSave] replace begin original=%s (%llu bytes) trimmed=%s (%llu bytes)",
		origPath.c_str(), static_cast<unsigned long long>(origSz), trimmedPath.c_str(),
		static_cast<unsigned long long>(trimSz));

	if (trimSz == 0) {
		obs_log(LOG_WARNING, "[ClipSave] replace aborted: trimmed file missing or empty path=%s",
			trimmedPath.c_str());
		return false;
	}

	overlay_window_release_file_if_open(origPath.c_str());

	std::filesystem::rename(trimmedPath, origPath, ec);
	if (ec) {
		std::filesystem::remove(origPath, ec);
		if (ec) {
			obs_log(LOG_ERROR, "[ClipSave] failed to delete original file path=%s err=%d",
				origPath.c_str(), ec.value());
			return false;
		}
		std::filesystem::rename(trimmedPath, origPath, ec);
		if (ec) {
			obs_log(LOG_ERROR,
				"[ClipSave] failed to move trimmed file into place from=%s to=%s err=%d",
				trimmedPath.c_str(), origPath.c_str(), ec.value());
			return false;
		}
	}

	uint64_t finalSz = static_cast<uint64_t>(std::filesystem::file_size(origPath, ec));
	obs_log(LOG_INFO, "[ClipSave] replace done path=%s final_size=%llu bytes", origPath.c_str(),
		static_cast<unsigned long long>(finalSz));
	return true;
}

void TrimClipKeepLastAsync(const std::string &filePath, int keepSeconds)
{
	obs_log(LOG_INFO, "[ClipSave] trim worker thread spawning keep=%.3f s path=%s",
		static_cast<double>(keepSeconds), filePath.c_str());

	std::thread([filePath, keepSeconds]() {
		auto t0 = std::chrono::steady_clock::now();
		obs_log(LOG_INFO, "[ClipSave] trim worker entered path=%s keep=%.3f s (waiting for trim mutex)",
			filePath.c_str(), static_cast<double>(keepSeconds));
		std::lock_guard<std::mutex> lock(g_trim_mutex);

		std::error_code ec;
		uint64_t sizeBytes = std::filesystem::exists(filePath, ec)
					     ? static_cast<uint64_t>(std::filesystem::file_size(filePath, ec))
					     : 0;
		obs_log(LOG_INFO, "[ClipSave] trim worker start path=%s keep=%.3f s size=%llu bytes",
			filePath.c_str(), static_cast<double>(keepSeconds),
			static_cast<unsigned long long>(sizeBytes));

		std::string tmpPath;
		PlaybackProbeResult preProbe;
		double plannedTrimStart = 0.0;
		std::string err;

		if (!OverlayTrimmer::TrimKeepLastSeconds(filePath, static_cast<double>(keepSeconds), 0.25,
							 tmpPath, &preProbe, &plannedTrimStart, err)) {
			auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
					       .count();
			obs_log(LOG_WARNING, "[ClipSave] trim failed path=%s remux_time=%.3f s error=%s",
				filePath.c_str(), elapsed, err.c_str());
			return;
		}

		if (tmpPath.empty()) {
			obs_log(LOG_INFO,
				"[ClipSave] trim skipped path=%s reason=keep_ge_file keep=%.3f span=%.3f",
				filePath.c_str(), static_cast<double>(keepSeconds), preProbe.spanSec);
			return;
		}

		if (!ReplaceFileWithTrimmed(filePath, tmpPath)) {
			obs_log(LOG_WARNING, "[ClipSave] trim replace failed path=%s", filePath.c_str());
			std::filesystem::remove(tmpPath, ec);
			return;
		}

		PlaybackProbeResult postProbe;
		OverlayTrimmer::ProbePlaybackRange(filePath, postProbe, false);
		auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		obs_log(LOG_INFO,
			"[ClipSave] trim done path=%s keep_target=%.3f actual=%.3f delta=%+.3f removed_planned=%.3f worker_time=%.3f s",
			filePath.c_str(), static_cast<double>(keepSeconds), postProbe.spanSec,
			postProbe.spanSec - keepSeconds, plannedTrimStart, elapsed);
	}).detach();
}

} // namespace

extern "C" {

int overlay_clip_save_get_buffer_length(void)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 0;
	const char *sec = ReplayBufferConfigSection(cfg);
	return static_cast<int>(config_get_int(cfg, sec, "RecRBTime"));
}

bool overlay_clip_save_is_pending(void)
{
	return g_pending_keep_seconds.load() > 0;
}

bool overlay_clip_save_request(int seconds)
{
	int bufLen = overlay_clip_save_get_buffer_length();
	bool rbActive = obs_frontend_replay_buffer_active();
	obs_log(LOG_INFO,
		"[ClipSave] request begin requested=%d s buffer_len=%d pending_before=%d replay_active=%s",
		seconds, bufLen, g_pending_keep_seconds.load(), rbActive ? "yes" : "no");

	if (seconds <= 0) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: non-positive duration (%d)", seconds);
		return false;
	}
	if (!rbActive) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: replay buffer inactive");
		return false;
	}

	int clamped = seconds;
	if (bufLen > 0 && seconds > bufLen) {
		obs_log(LOG_INFO, "[ClipSave] duration clamped %d -> %d s", seconds, bufLen);
		clamped = bufLen;
	}

	int expected = 0;
	if (!g_pending_keep_seconds.compare_exchange_strong(expected, clamped)) {
		obs_log(LOG_WARNING, "[ClipSave] request rejected: pending clip already set (%d s)", expected);
		return false;
	}

	obs_log(LOG_INFO, "[ClipSave] pending keep-last set to %d s; calling obs_frontend_replay_buffer_save()",
		clamped);
	obs_frontend_replay_buffer_save();
	return true;
}

bool overlay_clip_save_on_replay_saved(const char *path)
{
	int pending = g_pending_keep_seconds.exchange(0);
	std::error_code ec;
	bool exists = path && *path && std::filesystem::exists(path, ec);
	obs_log(LOG_INFO, "[ClipSave] on_replay_saved pending=%d s path=%s exists=%s", pending,
		path ? path : "(null)", exists ? "yes" : "no");

	if (pending <= 0) {
		obs_log(LOG_INFO, "[ClipSave] on_replay_saved: no pending keep-last duration");
		return false;
	}
	if (!exists) {
		obs_log(LOG_WARNING, "[ClipSave] pending %d s but saved path missing/invalid", pending);
		return true;
	}

	uint64_t sz = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
	obs_log(LOG_INFO, "[ClipSave] scheduling keep-last trim path=%s keep=%d s size=%llu bytes", path,
		pending, static_cast<unsigned long long>(sz));
	TrimClipKeepLastAsync(path, pending);
	return true;
}

} // extern "C"
