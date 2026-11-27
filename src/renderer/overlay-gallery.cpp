#include "overlay-gallery.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QUrl>

#include <algorithm>
#include <filesystem>

#include "overlay-string-utils.h"

namespace {

bool IsSupportedVideoExt(const std::string &extLower)
{
	return extLower == ".mp4" || extLower == ".mkv" || extLower == ".mov" || extLower == ".avi";
}

bool ExtractThumbAndDuration(const std::string &path, QImage &outImg, double &outDurSec)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt)
		return false;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	outDurSec = fmt->duration != AV_NOPTS_VALUE
			    ? std::max(0.0, static_cast<double>(fmt->duration) / AV_TIME_BASE)
			    : 0.0;

	int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	AVStream *st = fmt->streams[vIdx];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		avformat_close_input(&fmt);
		return false;
	}
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
	    avcodec_open2(dec, codec, nullptr) < 0) {
		avcodec_free_context(&dec);
		avformat_close_input(&fmt);
		return false;
	}

	if (outDurSec > 2.0) {
		double seekSec = std::min(1.5, outDurSec * 0.15);
		av_seek_frame(fmt, -1, static_cast<int64_t>(seekSec * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	SwsContext *sws = nullptr;
	bool got = false;
	int reads = 0;

	while (reads++ < 150 && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vIdx) {
			if (avcodec_send_packet(dec, pkt) >= 0) {
				if (avcodec_receive_frame(dec, frame) >= 0) {
					int dstW = 240;
					int dstH = frame->width > 0
							   ? std::max(2, frame->height * dstW / frame->width)
							   : 135;
					sws = sws_getCachedContext(sws, frame->width, frame->height,
								   static_cast<AVPixelFormat>(frame->format),
								   dstW, dstH, AV_PIX_FMT_BGRA, SWS_BILINEAR,
								   nullptr, nullptr, nullptr);
					if (sws) {
						QImage img(dstW, dstH, QImage::Format_ARGB32_Premultiplied);
						uint8_t *dstData[4] = {img.bits(), nullptr, nullptr, nullptr};
						int dstLine[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
						sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
							  dstData, dstLine);
						outImg = std::move(img);
						got = true;
					}
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(pkt);
		if (got)
			break;
	}

	if (sws)
		sws_freeContext(sws);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	return got;
}

} // namespace

OverlayGallery::OverlayGallery() = default;

OverlayGallery::~OverlayGallery()
{
	StopWorkers();
}

void OverlayGallery::StopWorkers()
{
	stop_ = true;
	cv_.notify_all();
	if (worker_.joinable())
		worker_.join();
	workerRunning_ = false;
}

void OverlayGallery::StartWorkerIfNeeded()
{
	if (!workerRunning_) {
		stop_ = false;
		workerRunning_ = true;
		worker_ = std::thread(&OverlayGallery::WorkerLoop, this);
	}
}

void OverlayGallery::Refresh(const std::string &directory, std::function<void()> onThumbReady)
{
	std::vector<GalleryItem> found;
	std::error_code ec;
	if (!directory.empty() && std::filesystem::exists(directory, ec)) {
		for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
			if (!entry.is_regular_file(ec))
				continue;
			std::string ext = overlay_str::ToLowerAscii(entry.path().extension().string());
			if (!IsSupportedVideoExt(ext))
				continue;
			GalleryItem item;
			item.filePath = entry.path().string();
			item.fileName = entry.path().filename().string();
			item.extension = ext;
			item.sizeBytes = static_cast<uint64_t>(entry.file_size(ec));
			auto ftime = entry.last_write_time(ec);
			auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
			item.mtimeEpochSec = std::chrono::duration_cast<std::chrono::seconds>(
						     sctp.time_since_epoch())
						     .count();
			found.push_back(std::move(item));
		}
	}

	std::sort(found.begin(), found.end(), [](const GalleryItem &a, const GalleryItem &b) {
		return a.mtimeEpochSec > b.mtimeEpochSec;
	});

	{
		std::lock_guard<std::mutex> lock(mutex_);
		currentDir_ = directory;
		onThumbReady_ = std::move(onThumbReady);

		// Preserve already loaded thumbnails if paths match
		for (auto &newItem : found) {
			for (const auto &oldItem : items_) {
				if (oldItem.filePath == newItem.filePath &&
				    oldItem.sizeBytes == newItem.sizeBytes && oldItem.thumbLoaded) {
					newItem.thumbnail = oldItem.thumbnail;
					newItem.durationSec = oldItem.durationSec;
					newItem.thumbLoaded = true;
					break;
				}
			}
		}

		items_ = std::move(found);
		thumbQueue_.clear();
		for (auto &it : items_) {
			if (!it.thumbLoaded) {
				it.thumbRequested = true;
				thumbQueue_.push_back(it.filePath);
			}
		}
		StartWorkerIfNeeded();
	}
	cv_.notify_one();
}

void OverlayGallery::WorkerLoop()
{
	while (!stop_) {
		std::string targetPath;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [&]() { return stop_ || !thumbQueue_.empty(); });
			if (stop_)
				break;
			targetPath = std::move(thumbQueue_.front());
			thumbQueue_.pop_front();
		}

		QImage img;
		double dur = 0.0;
		bool ok = ExtractThumbAndDuration(targetPath, img, dur);

		std::function<void()> cb;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &it : items_) {
				if (it.filePath == targetPath) {
					if (ok)
						it.thumbnail = std::move(img);
					it.durationSec = dur;
					it.thumbLoaded = true;
					break;
				}
			}
			cb = onThumbReady_;
		}
		if (cb)
			cb();
	}
}

std::vector<GalleryItem> OverlayGallery::SnapshotItems() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return items_;
}

bool OverlayGallery::GetThumbnail(const std::string &filePath, QImage &outThumb) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &it : items_) {
		if (it.filePath == filePath && !it.thumbnail.isNull()) {
			outThumb = it.thumbnail;
			return true;
		}
	}
	return false;
}

size_t OverlayGallery::ItemCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return items_.size();
}

bool OverlayGallery::OpenInFileManager(const std::string &filePath)
{
	if (filePath.empty())
		return false;
	std::filesystem::path p(filePath);
	std::string dir = std::filesystem::is_directory(p) ? p.string() : p.parent_path().string();
	if (dir.empty())
		dir = ".";
	return QProcess::startDetached(QStringLiteral("xdg-open"), {overlay_str::Utf8ToQString(dir)});
}

bool OverlayGallery::OpenWithDefaultApp(const std::string &filePath)
{
	if (filePath.empty())
		return false;
	return QProcess::startDetached(QStringLiteral("xdg-open"),
				       {overlay_str::Utf8ToQString(filePath)});
}

void OverlayGallery::CopyPathToClipboard(const std::string &filePath)
{
	if (QClipboard *cb = QGuiApplication::clipboard())
		cb->setText(overlay_str::Utf8ToQString(filePath));
}

void OverlayGallery::CopyFileToClipboard(const std::string &filePath)
{
	if (QClipboard *cb = QGuiApplication::clipboard()) {
		auto *mime = new QMimeData();
		QList<QUrl> urls{QUrl::fromLocalFile(overlay_str::Utf8ToQString(filePath))};
		mime->setUrls(urls);
		QByteArray gnomePayload =
			"copy\n" + urls[0].toEncoded();
		mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomePayload);
		cb->setMimeData(mime);
	}
}

bool OverlayGallery::RenameMediaFile(const std::string &oldPath, const std::string &newStem,
				     std::string &outNewPath, std::string &outError)
{
	std::string trimmed = overlay_str::TrimAscii(newStem);
	if (trimmed.empty() || trimmed == "." || trimmed == ".." ||
	    trimmed.find('/') != std::string::npos || trimmed.find('\\') != std::string::npos ||
	    trimmed.find(':') != std::string::npos) {
		outError = "Gallery.Ctx.RenameInvalid";
		return false;
	}

	std::filesystem::path p(oldPath);
	std::filesystem::path np = p.parent_path() / (trimmed + p.extension().string());
	std::error_code ec;
	if (np == p) {
		outNewPath = oldPath;
		return true;
	}
	if (std::filesystem::exists(np, ec)) {
		outError = "Gallery.Ctx.RenameExists";
		return false;
	}
	std::filesystem::rename(p, np, ec);
	if (ec) {
		outError = "Gallery.Ctx.RenameFailed";
		return false;
	}
	outNewPath = np.string();
	return true;
}

bool OverlayGallery::DeleteMediaFile(const std::string &filePath)
{
	if (filePath.empty())
		return false;
	std::error_code ec;
	return std::filesystem::remove(filePath, ec);
}
