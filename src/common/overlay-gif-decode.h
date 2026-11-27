#pragma once

#include <QImage>
#include <cstdint>
#include <string>
#include <vector>

namespace overlay::gif {

struct DecodedFrame {
	QImage image; // Format_ARGB32_Premultiplied
	uint32_t delayMs = 100;
};

bool DecodeGifFile(const std::string &path, std::vector<DecodedFrame> &outFrames, int maxFrames = 512);

} // namespace overlay::gif
