#pragma once

#include <string>
#include <vector>

namespace overlay_mp4 {

// Reads per-track MP4 udta/name strings for audio ('soun') tracks in container order.
std::vector<std::string> ReadMp4AudioTrackNames(const std::string &filePath);

} // namespace overlay_mp4
