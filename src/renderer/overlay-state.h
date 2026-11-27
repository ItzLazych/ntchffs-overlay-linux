#pragma once

#include <cstdint>
#include <string>

struct OverlayState {
	bool recordingActive = false;
	bool recordingPaused = false;
	bool replayActive = false;
	bool replaySaving = false;
	double fps = 0.0;
	int droppedFrames = 0;
	int totalFrames = 0;
	std::string freeSpaceText;
	uint64_t freeSpaceBytes = 0;
	bool statsVisible = true;
	bool recordingPathConfigured = false;
	std::string recordingDir;
	int replayBufferSeconds = 0;
};

class OverlayStateManager {
public:
	static OverlayState Refresh();
	static std::string ResolveObsRecordingDirectory();
	static uint64_t QueryFreeDiskSpaceBytes(const std::string &dirPath);
	static std::string FormatFreeDiskSpace(uint64_t freeBytes);
};
