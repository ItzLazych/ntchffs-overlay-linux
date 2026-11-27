#include "overlay-icon-frames.h"

#include <QImageReader>
#include <algorithm>
#include <filesystem>

#include "overlay-gif-decode.h"
#include "overlay-string-utils.h"
#include "overlay-svg-rasterize.h"

namespace overlay::icon {

QImage ApplyPremultipliedTint(const QImage &src, uint32_t rgb)
{
	if (src.isNull())
		return {};
	QImage img = src.format() == QImage::Format_ARGB32_Premultiplied
			     ? src.copy()
			     : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const uint8_t tr = static_cast<uint8_t>((rgb >> 16) & 0xFF);
	const uint8_t tg = static_cast<uint8_t>((rgb >> 8) & 0xFF);
	const uint8_t tb = static_cast<uint8_t>(rgb & 0xFF);

	const int w = img.width();
	const int h = img.height();
	for (int y = 0; y < h; ++y) {
		QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int x = 0; x < w; ++x) {
			uint32_t px = line[x];
			uint32_t a = (px >> 24) & 0xFF;
			if (a == 0) {
				line[x] = 0;
				continue;
			}
			uint32_t r = (tr * a + 127) / 255;
			uint32_t g = (tg * a + 127) / 255;
			uint32_t b = (tb * a + 127) / 255;
			line[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	return img;
}

void Slot::Clear()
{
	loadedPath.clear();
	tintEnabled = false;
	tintRgb = 0xFFFFFF;
	frames.clear();
	currentFrame = 0;
	nextAdvanceMs = 0;
}

bool Slot::LoadFromFile(const std::string &path, bool tint, uint32_t rgb, int rasterTargetPx)
{
	if (path == loadedPath && tint == tintEnabled && rgb == tintRgb && !frames.empty())
		return true;

	Clear();
	loadedPath = path;
	tintEnabled = tint;
	tintRgb = rgb;

	if (path.empty())
		return false;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return false;

	std::string ext = overlay_str::ExtensionLowerFromPath(path);
	auto addImageFrame = [&](QImage img, uint32_t delayMs) {
		if (img.isNull())
			return;
		if (tintEnabled)
			img = ApplyPremultipliedTint(img, tintRgb);
		else if (img.format() != QImage::Format_ARGB32_Premultiplied)
			img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		Frame f;
		f.bitmap = std::make_shared<overlay_compat::Bitmap>(std::move(img));
		f.delayMs = std::max<uint32_t>(20, delayMs);
		frames.push_back(std::move(f));
	};

	if (ext == ".svg") {
		QImage img;
		if (!overlay::svg::RasterizeSvgFile(path, rasterTargetPx, rasterTargetPx, img))
			return false;
		addImageFrame(std::move(img), 100);
		return !frames.empty();
	}

	if (ext == ".gif") {
		std::vector<overlay::gif::DecodedFrame> gifFrames;
		if (overlay::gif::DecodeGifFile(path, gifFrames)) {
			for (auto &gf : gifFrames)
				addImageFrame(std::move(gf.image), gf.delayMs);
			if (!frames.empty()) {
				nextAdvanceMs = overlay_compat::GetTickCount64Ms() + frames[0].delayMs;
				return true;
			}
		}
	}

	QImageReader reader(overlay_str::Utf8ToQString(path));
	reader.setAutoTransform(true);
	QImage img = reader.read();
	if (img.isNull())
		return false;
	addImageFrame(std::move(img), 100);
	return !frames.empty();
}

const overlay_compat::Bitmap *Slot::CurrentBitmap(uint64_t nowMs)
{
	if (frames.empty())
		return nullptr;
	if (frames.size() > 1) {
		if (nextAdvanceMs == 0)
			nextAdvanceMs = nowMs + frames[currentFrame].delayMs;
		else if (nowMs >= nextAdvanceMs) {
			currentFrame = (currentFrame + 1) % frames.size();
			nextAdvanceMs = nowMs + frames[currentFrame].delayMs;
		}
	}
	return frames[currentFrame].bitmap.get();
}

uint64_t Slot::NextTimerDelayMs(uint64_t nowMs) const
{
	if (frames.size() <= 1 || nextAdvanceMs == 0)
		return 0;
	if (nowMs >= nextAdvanceMs)
		return 16;
	return nextAdvanceMs - nowMs;
}

} // namespace overlay::icon
