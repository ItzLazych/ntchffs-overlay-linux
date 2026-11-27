#include "overlay-state.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <util/config-file.h>

#include <cstdio>
#include <filesystem>

std::string OverlayStateManager::ResolveObsRecordingDirectory()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return {};

	const char *mode = config_get_string(cfg, "Output", "Mode");
	const char *path = nullptr;
	if (mode && std::string(mode) == "Advanced") {
		const char *recType = config_get_string(cfg, "AdvOut", "RecType");
		if (recType && std::string(recType) == "FFmpeg")
			path = config_get_string(cfg, "AdvOut", "FFFilePath");
		else
			path = config_get_string(cfg, "AdvOut", "RecFilePath");
	} else {
		path = config_get_string(cfg, "SimpleOutput", "FilePath");
	}
	return path ? std::string(path) : std::string{};
}

uint64_t OverlayStateManager::QueryFreeDiskSpaceBytes(const std::string &dirPath)
{
	if (dirPath.empty())
		return 0;
	std::error_code ec;
	std::filesystem::path p(dirPath);
	while (!p.empty() && !std::filesystem::exists(p, ec))
		p = p.parent_path();
	if (p.empty())
		return 0;
	auto sp = std::filesystem::space(p, ec);
	if (ec || sp.available == static_cast<uintmax_t>(-1))
		return 0;
	return static_cast<uint64_t>(sp.available);
}

std::string OverlayStateManager::FormatFreeDiskSpace(uint64_t freeBytes)
{
	const char *prefix = obs_module_text("Stats.FreeSpacePrefix");
	const char *gbStr = obs_module_text("Stats.GB");
	const char *unkStr = obs_module_text("Stats.FreeSpaceUnknown");
	if (!prefix)
		prefix = "Free:";
	if (!gbStr)
		gbStr = "GB";
	if (!unkStr)
		unkStr = "-";

	if (freeBytes == 0)
		return std::string(prefix) + " " + unkStr;

	double gb = static_cast<double>(freeBytes) / (1024.0 * 1024.0 * 1024.0);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%s %.1f %s", prefix, gb, gbStr);
	return buf;
}

OverlayState OverlayStateManager::Refresh()
{
	OverlayState st;
	st.recordingActive = obs_frontend_recording_active();
	st.recordingPaused = obs_frontend_recording_paused();
	st.replayActive = obs_frontend_replay_buffer_active();
	st.fps = obs_get_active_fps();

	obs_output_t *out = obs_frontend_get_recording_output();
	if (out) {
		st.totalFrames = static_cast<int>(obs_output_get_total_frames(out));
		st.droppedFrames = static_cast<int>(obs_output_get_frames_dropped(out));
		obs_output_release(out);
	} else {
		st.totalFrames = static_cast<int>(obs_get_total_frames());
		st.droppedFrames = static_cast<int>(obs_get_lagged_frames());
	}

	st.recordingDir = ResolveObsRecordingDirectory();
	st.recordingPathConfigured = !st.recordingDir.empty();
	st.freeSpaceBytes = QueryFreeDiskSpaceBytes(st.recordingDir);
	st.freeSpaceText = FormatFreeDiskSpace(st.freeSpaceBytes);

	if (config_t *cfg = obs_frontend_get_profile_config()) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		const char *sec = (mode && std::string(mode) == "Advanced") ? "AdvOut" : "SimpleOutput";
		st.replayBufferSeconds = static_cast<int>(config_get_int(cfg, sec, "RecRBTime"));
	}

	return st;
}
