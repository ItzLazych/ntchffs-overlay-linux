#include "overlay-gif-decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace overlay::gif {

bool DecodeGifFile(const std::string &path, std::vector<DecodedFrame> &outFrames, int maxFrames)
{
	outFrames.clear();
	if (path.empty())
		return false;

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt)
		return false;

	auto cleanupFmt = [&]() {
		if (fmt)
			avformat_close_input(&fmt);
	};

	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		cleanupFmt();
		return false;
	}

	int streamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (streamIdx < 0) {
		cleanupFmt();
		return false;
	}

	AVStream *st = fmt->streams[streamIdx];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		cleanupFmt();
		return false;
	}

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx) {
		cleanupFmt();
		return false;
	}

	auto cleanupAll = [&]() {
		avcodec_free_context(&ctx);
		cleanupFmt();
	};

	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
		cleanupAll();
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	SwsContext *sws = nullptr;
	if (!pkt || !frame) {
		av_packet_free(&pkt);
		av_frame_free(&frame);
		cleanupAll();
		return false;
	}

	auto extractFrame = [&](const AVFrame *decoded, uint32_t delayMs) {
		if (decoded->width <= 0 || decoded->height <= 0)
			return;
		sws = sws_getCachedContext(sws, decoded->width, decoded->height,
					   static_cast<AVPixelFormat>(decoded->format), decoded->width,
					   decoded->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
					   nullptr);
		if (!sws)
			return;

		QImage img(decoded->width, decoded->height, QImage::Format_ARGB32);
		uint8_t *dstData[4] = {img.bits(), nullptr, nullptr, nullptr};
		int dstLinesize[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
		sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height, dstData, dstLinesize);

		DecodedFrame df;
		df.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		df.delayMs = delayMs < 15 ? 100 : delayMs;
		outFrames.push_back(std::move(df));
	};

	while (static_cast<int>(outFrames.size()) < maxFrames && av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == streamIdx) {
			int64_t pktDelay = 100;
			if (pkt->duration > 0) {
				pktDelay = av_rescale_q(pkt->duration, st->time_base, AVRational{1, 1000});
			}
			uint32_t delayMs = static_cast<uint32_t>(std::clamp<int64_t>(pktDelay, 10, 10000));

			if (avcodec_send_packet(ctx, pkt) >= 0) {
				while (avcodec_receive_frame(ctx, frame) >= 0) {
					extractFrame(frame, delayMs);
					av_frame_unref(frame);
					if (static_cast<int>(outFrames.size()) >= maxFrames)
						break;
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (static_cast<int>(outFrames.size()) < maxFrames && avcodec_send_packet(ctx, nullptr) >= 0) {
		while (avcodec_receive_frame(ctx, frame) >= 0) {
			extractFrame(frame, 100);
			av_frame_unref(frame);
			if (static_cast<int>(outFrames.size()) >= maxFrames)
				break;
		}
	}

	if (sws)
		sws_freeContext(sws);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	cleanupAll();
	return !outFrames.empty();
}

} // namespace overlay::gif
