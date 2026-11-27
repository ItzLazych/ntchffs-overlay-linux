#pragma once

#include <QColor>
#include <QImage>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "win-d2d-compat.h"

namespace overlay::icon {

struct Frame {
	std::shared_ptr<overlay_compat::Bitmap> bitmap;
	uint32_t delayMs = 100;
};

struct Slot {
	std::string loadedPath;
	bool tintEnabled = false;
	uint32_t tintRgb = 0xFFFFFF;
	std::vector<Frame> frames;
	size_t currentFrame = 0;
	uint64_t nextAdvanceMs = 0;

	void Clear();
	bool LoadFromFile(const std::string &path, bool tint, uint32_t rgb, int rasterTargetPx = 64);
	bool IsLoaded() const { return !frames.empty(); }
	bool IsAnimated() const { return frames.size() > 1; }
	const overlay_compat::Bitmap *CurrentBitmap(uint64_t nowMs);
	uint64_t NextTimerDelayMs(uint64_t nowMs) const;
};

QImage ApplyPremultipliedTint(const QImage &src, uint32_t rgb);

} // namespace overlay::icon
