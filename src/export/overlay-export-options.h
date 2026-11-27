#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class RebuildMode {
	Copy = 0,
	Reencode = 1,
};

enum class RebuildOutputFormat {
	Mp4 = 0,
	Mov = 1,
	Gif = 2,
};

enum class RebuildQuality {
	Low = 0,
	Medium = 1,
	High = 2,
	VeryHigh = 3,
};

struct OverlayExportOptions {
	RebuildMode mode = RebuildMode::Reencode;
	RebuildOutputFormat format = RebuildOutputFormat::Mp4;
	int crf = 23;
	RebuildQuality quality = RebuildQuality::Medium;
	int width = 0;  // 0 = Source
	int height = 0; // 0 = Source / preserve aspect
	double fps = 0.0;
	bool keepAudio = true;
	bool keepVideo = true;
	bool tracksConfigured = false;
	std::vector<int> keepAudioOrdinals;

	int gifWidth = 480;
	int gifHeight = 0;
	int gifFps = 15;
	int gifColors = 128;
	int gifDither = 1;
	bool gifLoop = true;
};

struct OverlayExportSegment {
	std::string inputPath;
	double startSeconds = 0.0;
	double endSeconds = 0.0;
	std::vector<int> streamIndicesToKeep;
	OverlayExportOptions options;
};
