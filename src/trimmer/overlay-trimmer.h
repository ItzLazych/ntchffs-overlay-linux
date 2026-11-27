#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "overlay-export-options.h"

struct PlaybackProbeResult {
	bool valid = false;
	uint64_t sizeBytes = 0;
	double containerDuration = 0.0;
	double startSec = 0.0;
	double endSec = 0.0;
	double spanSec = 0.0;
	int refVideoStream = -1;
	int videoPackets = 0;
	std::string error;
};

class OverlayTrimmer {
public:
	using ProgressCallback = std::function<void(float progress01)>;

	static bool ProbePlaybackRange(const std::string &filePath, PlaybackProbeResult &out,
				       bool fullPacketScan = false);

	static bool ExportSegment(const OverlayExportSegment &segment, const std::string &outputPath,
				  std::atomic<bool> *cancelFlag, const ProgressCallback &progressCb,
				  std::string &outError);

	static bool ExportSegmentsMerged(const std::vector<OverlayExportSegment> &segments,
					   const std::string &outputPath,
					   const OverlayExportOptions &finalOptions,
					   std::atomic<bool> *cancelFlag, const ProgressCallback &progressCb,
					   std::string &outError);

	static bool TrimKeepLastSeconds(const std::string &filePath, double keepSeconds, double epsilonSec,
					std::string &outTempPath, PlaybackProbeResult *preProbeOut,
					double *outPlannedTrimStart, std::string &outError);
};
