#include "overlay-trimmer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

#include "overlay-ffmpeg-time-utils.h"
#include "overlay-ffmpeg-video-decode.h"

namespace {

std::string AvErrStr(int errnum)
{
	char buf[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(errnum, buf, sizeof(buf));
	return std::string(buf);
}

bool IsStreamSelected(int streamIdx, int audioOrdinal, bool isVideo, bool isAudio,
		      const OverlayExportOptions &opt, const std::vector<int> *explicitStreams)
{
	if (isVideo)
		return opt.keepVideo;
	if (!isAudio)
		return false;
	if (!opt.keepAudio)
		return false;
	if (explicitStreams && !explicitStreams->empty()) {
		return std::find(explicitStreams->begin(), explicitStreams->end(), streamIdx) !=
		       explicitStreams->end();
	}
	if (opt.tracksConfigured && !opt.keepAudioOrdinals.empty()) {
		return std::find(opt.keepAudioOrdinals.begin(), opt.keepAudioOrdinals.end(), audioOrdinal) !=
		       opt.keepAudioOrdinals.end();
	}
	return !opt.tracksConfigured;
}

bool ExportStreamCopy(AVFormatContext *inFmt, const OverlayExportSegment &segment, const std::string &outputPath,
		      double startSec, double endSec, std::atomic<bool> *cancelFlag,
		      const OverlayTrimmer::ProgressCallback &progressCb, std::string &outError)
{
	AVFormatContext *outFmt = nullptr;
	int ret = avformat_alloc_output_context2(&outFmt, nullptr, nullptr, outputPath.c_str());
	if (ret < 0 || !outFmt) {
		outError = "FFmpeg: failed to create output context";
		return false;
	}

	auto cleanupOut = [&]() {
		if (outFmt) {
			if (!(outFmt->oformat->flags & AVFMT_NOFILE) && outFmt->pb)
				avio_closep(&outFmt->pb);
			avformat_free_context(outFmt);
			outFmt = nullptr;
		}
	};

	std::vector<int> streamMap(inFmt->nb_streams, -1);
	std::vector<bool> sawFirstPacket(inFmt->nb_streams, false);
	std::vector<int64_t> firstDts(inFmt->nb_streams, AV_NOPTS_VALUE);
	std::vector<int64_t> lastWrittenDts(inFmt->nb_streams, AV_NOPTS_VALUE);
	std::vector<int64_t> lastWrittenPts(inFmt->nb_streams, AV_NOPTS_VALUE);

	int audioOrd = 0;
	int refStream = -1;
	int mappedCount = 0;
	for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
		AVStream *inSt = inFmt->streams[i];
		auto typ = inSt->codecpar->codec_type;
		bool isV = (typ == AVMEDIA_TYPE_VIDEO);
		bool isA = (typ == AVMEDIA_TYPE_AUDIO);
		int curAudioOrd = isA ? audioOrd++ : -1;

		if (!isV && !isA)
			continue;
		if (!IsStreamSelected(static_cast<int>(i), curAudioOrd, isV, isA, segment.options,
				      &segment.streamIndicesToKeep)) {
			continue;
		}

		AVStream *outSt = avformat_new_stream(outFmt, nullptr);
		if (!outSt) {
			outError = "FFmpeg: avformat_new_stream failed";
			cleanupOut();
			return false;
		}
		if (avcodec_parameters_copy(outSt->codecpar, inSt->codecpar) < 0) {
			outError = "FFmpeg: avcodec_parameters_copy failed";
			cleanupOut();
			return false;
		}
		outSt->codecpar->codec_tag = 0;
		outSt->time_base = inSt->time_base;
		streamMap[i] = mappedCount++;
		if (refStream < 0 && isV)
			refStream = static_cast<int>(i);
	}

	if (mappedCount == 0) {
		outError = "FFmpeg: no streams selected";
		cleanupOut();
		return false;
	}
	if (refStream < 0) {
		for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
			if (streamMap[i] >= 0) {
				refStream = static_cast<int>(i);
				break;
			}
		}
	}

	if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			outError = "FFmpeg: failed to open output file: " + AvErrStr(ret);
			cleanupOut();
			return false;
		}
	}

	AVDictionary *muxOpts = nullptr;
	if (segment.options.format == RebuildOutputFormat::Mp4 ||
	    segment.options.format == RebuildOutputFormat::Mov) {
		av_dict_set(&muxOpts, "movflags", "+faststart", 0);
	}
	ret = avformat_write_header(outFmt, &muxOpts);
	av_dict_free(&muxOpts);
	if (ret < 0) {
		outError = "FFmpeg: avformat_write_header failed: " + AvErrStr(ret);
		cleanupOut();
		return false;
	}

	if (startSec > 0.0) {
		int64_t seekTarget = static_cast<int64_t>(std::llround(startSec * AV_TIME_BASE));
		av_seek_frame(inFmt, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
	}

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		outError = "FFmpeg: av_packet_alloc failed";
		cleanupOut();
		return false;
	}

	const double totalSpan = std::max(0.1, endSec - startSec);
	int64_t anchorRefPts = AV_NOPTS_VALUE;
	AVRational anchorRefTb{1, AV_TIME_BASE};
	bool startedRef = false;

	while (av_read_frame(inFmt, pkt) >= 0) {
		if (cancelFlag && cancelFlag->load()) {
			av_packet_unref(pkt);
			av_packet_free(&pkt);
			cleanupOut();
			outError = "Cancelled";
			return false;
		}

		int inIdx = pkt->stream_index;
		if (inIdx < 0 || static_cast<size_t>(inIdx) >= streamMap.size() || streamMap[inIdx] < 0) {
			av_packet_unref(pkt);
			continue;
		}

		AVStream *inSt = inFmt->streams[inIdx];
		AVStream *outSt = outFmt->streams[streamMap[inIdx]];
		int64_t ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
		double pktSec = ts != AV_NOPTS_VALUE ? overlay::ffmpeg::PtsToSeconds(ts, inSt->time_base) : 0.0;

		if (inIdx == refStream && inSt->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !startedRef) {
			if (pktSec < startSec - 0.05 && !(pkt->flags & AV_PKT_FLAG_KEY)) {
				// Before start and waiting for keyframe or target
			}
			// Wait for keyframe near/after start if possible or take the seeked keyframe
			if ((pkt->flags & AV_PKT_FLAG_KEY) || pktSec >= startSec) {
				startedRef = true;
				anchorRefPts = ts;
				anchorRefTb = inSt->time_base;
			} else {
				av_packet_unref(pkt);
				continue;
			}
		}

		if (endSec > 0.0 && pktSec > endSec + 0.02) {
			if (inIdx == refStream) {
				av_packet_unref(pkt);
				break;
			}
			av_packet_unref(pkt);
			continue;
		}

		if (inIdx != refStream && !startedRef && pktSec < startSec) {
			av_packet_unref(pkt);
			continue;
		}

		if (!sawFirstPacket[inIdx]) {
			sawFirstPacket[inIdx] = true;
			if (anchorRefPts != AV_NOPTS_VALUE) {
				firstDts[inIdx] = av_rescale_q(anchorRefPts, anchorRefTb, inSt->time_base);
			} else {
				firstDts[inIdx] = (ts != AV_NOPTS_VALUE) ? ts : 0;
			}
		}

		int64_t offset = firstDts[inIdx];
		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts -= offset;
		if (pkt->dts != AV_NOPTS_VALUE)
			pkt->dts -= offset;
		if (pkt->pts < 0 && pkt->pts != AV_NOPTS_VALUE)
			pkt->pts = 0;
		if (pkt->dts < 0 && pkt->dts != AV_NOPTS_VALUE)
			pkt->dts = 0;

		av_packet_rescale_ts(pkt, inSt->time_base, outSt->time_base);

		if (pkt->dts != AV_NOPTS_VALUE) {
			if (lastWrittenDts[inIdx] != AV_NOPTS_VALUE && pkt->dts <= lastWrittenDts[inIdx])
				pkt->dts = lastWrittenDts[inIdx] + 1;
			if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
				pkt->pts = pkt->dts;
			lastWrittenDts[inIdx] = pkt->dts;
		} else if (pkt->pts != AV_NOPTS_VALUE) {
			if (lastWrittenPts[inIdx] != AV_NOPTS_VALUE && pkt->pts <= lastWrittenPts[inIdx])
				pkt->pts = lastWrittenPts[inIdx] + 1;
			lastWrittenPts[inIdx] = pkt->pts;
			pkt->dts = pkt->pts;
		}

		pkt->stream_index = streamMap[inIdx];
		pkt->pos = -1;

		ret = av_interleaved_write_frame(outFmt, pkt);
		av_packet_unref(pkt);
		if (ret < 0) {
			outError = "FFmpeg: write failed: " + AvErrStr(ret);
			av_packet_free(&pkt);
			cleanupOut();
			return false;
		}

		if (progressCb && inIdx == refStream) {
			float p = static_cast<float>(std::clamp((pktSec - startSec) / totalSpan, 0.0, 0.99));
			progressCb(p);
		}
	}

	av_packet_free(&pkt);
	ret = av_write_trailer(outFmt);
	cleanupOut();
	if (ret < 0) {
		outError = "FFmpeg: av_write_trailer failed: " + AvErrStr(ret);
		return false;
	}
	if (progressCb)
		progressCb(1.0f);
	return true;
}

bool ExportGif(AVFormatContext *inFmt, const OverlayExportSegment &segment, const std::string &outputPath,
	       double startSec, double endSec, std::atomic<bool> *cancelFlag,
	       const OverlayTrimmer::ProgressCallback &progressCb, std::string &outError)
{
	int vIdx = av_find_best_stream(inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		outError = "FFmpeg: no video stream for GIF export";
		return false;
	}

	AVStream *inVSt = inFmt->streams[vIdx];
	const AVCodec *dec = avcodec_find_decoder(inVSt->codecpar->codec_id);
	if (!dec) {
		outError = "FFmpeg: video decoder not found";
		return false;
	}

	AVCodecContext *decCtx = avcodec_alloc_context3(dec);
	if (!decCtx || avcodec_parameters_to_context(decCtx, inVSt->codecpar) < 0 ||
	    avcodec_open2(decCtx, dec, nullptr) < 0) {
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: failed to open video decoder";
		return false;
	}

	const AVCodec *gifCodec = avcodec_find_encoder(AV_CODEC_ID_GIF);
	if (!gifCodec) {
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: GIF encoder not found in this build";
		return false;
	}

	AVFormatContext *outFmt = nullptr;
	if (avformat_alloc_output_context2(&outFmt, nullptr, "gif", outputPath.c_str()) < 0 || !outFmt) {
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: failed to create GIF output context";
		return false;
	}

	int srcW = std::max(2, decCtx->width);
	int srcH = std::max(2, decCtx->height);
	int dstW = segment.options.gifWidth > 0 ? segment.options.gifWidth : 480;
	int dstH = segment.options.gifHeight > 0
			   ? segment.options.gifHeight
			   : std::max(2, static_cast<int>(std::lround(static_cast<double>(dstW) * srcH / srcW)));
	dstW = std::max(2, dstW & ~1);
	dstH = std::max(2, dstH & ~1);
	int gifFps = std::clamp(segment.options.gifFps, 1, 50);

	AVCodecContext *encCtx = avcodec_alloc_context3(gifCodec);
	if (!encCtx) {
		avformat_free_context(outFmt);
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: avcodec_alloc_context3(gif) failed";
		return false;
	}
	encCtx->width = dstW;
	encCtx->height = dstH;
	encCtx->pix_fmt = AV_PIX_FMT_RGB8;
	encCtx->time_base = AVRational{1, gifFps};

	if (avcodec_open2(encCtx, gifCodec, nullptr) < 0) {
		avcodec_free_context(&encCtx);
		avformat_free_context(outFmt);
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: avcodec_open2(gif) failed";
		return false;
	}

	AVStream *outSt = avformat_new_stream(outFmt, nullptr);
	if (!outSt || avcodec_parameters_from_context(outSt->codecpar, encCtx) < 0) {
		avcodec_free_context(&encCtx);
		avformat_free_context(outFmt);
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: avcodec_parameters_from_context(gif) failed";
		return false;
	}
	outSt->time_base = encCtx->time_base;

	if (avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
		avcodec_free_context(&encCtx);
		avformat_free_context(outFmt);
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: failed to open GIF output";
		return false;
	}

	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "loop", segment.options.gifLoop ? "0" : "-1", 0);
	int ret = avformat_write_header(outFmt, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		avio_closep(&outFmt->pb);
		avcodec_free_context(&encCtx);
		avformat_free_context(outFmt);
		avcodec_free_context(&decCtx);
		outError = "FFmpeg: avformat_write_header(gif) failed: " + AvErrStr(ret);
		return false;
	}

	if (startSec > 0.0) {
		av_seek_frame(inFmt, -1, static_cast<int64_t>(std::llround(startSec * AV_TIME_BASE)),
			      AVSEEK_FLAG_BACKWARD);
	}

	AVPacket *pkt = av_packet_alloc();
	AVPacket *encPkt = av_packet_alloc();
	AVFrame *decFrame = av_frame_alloc();
	AVFrame *encFrame = av_frame_alloc();
	encFrame->format = encCtx->pix_fmt;
	encFrame->width = dstW;
	encFrame->height = dstH;
	av_frame_get_buffer(encFrame, 32);

	SwsContext *sws = nullptr;
	const double frameStepSec = 1.0 / gifFps;
	double nextSampleSec = startSec;
	int64_t frameIndex = 0;
	const double totalSpan = std::max(0.1, endSec - startSec);
	bool aborted = false;

	auto flushEnc = [&]() {
		while (avcodec_receive_packet(encCtx, encPkt) >= 0) {
			av_packet_rescale_ts(encPkt, encCtx->time_base, outSt->time_base);
			encPkt->stream_index = 0;
			av_interleaved_write_frame(outFmt, encPkt);
			av_packet_unref(encPkt);
		}
	};

	auto pushDecoded = [&](AVFrame *f) {
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		double sec = overlay::ffmpeg::PtsToSeconds(pts, inVSt->time_base);
		if (sec < startSec - 0.01)
			return true;
		if (endSec > 0.0 && sec > endSec + 0.01)
			return false;
		if (sec + 0.002 < nextSampleSec)
			return true;

		while (nextSampleSec <= sec + 0.002)
			nextSampleSec += frameStepSec;

		sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format),
					   dstW, dstH, encCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws)
			return false;
		av_frame_make_writable(encFrame);
		sws_scale(sws, f->data, f->linesize, 0, f->height, encFrame->data, encFrame->linesize);
		encFrame->pts = frameIndex++;
		if (avcodec_send_frame(encCtx, encFrame) >= 0)
			flushEnc();
		if (progressCb)
			progressCb(static_cast<float>(std::clamp((sec - startSec) / totalSpan, 0.0, 0.99)));
		return true;
	};

	bool stopReading = false;
	while (!stopReading && av_read_frame(inFmt, pkt) >= 0) {
		if (cancelFlag && cancelFlag->load()) {
			aborted = true;
			av_packet_unref(pkt);
			break;
		}
		if (pkt->stream_index == vIdx) {
			if (avcodec_send_packet(decCtx, pkt) >= 0) {
				while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
					if (!pushDecoded(decFrame))
						stopReading = true;
					av_frame_unref(decFrame);
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (!aborted && !stopReading && avcodec_send_packet(decCtx, nullptr) >= 0) {
		while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
			pushDecoded(decFrame);
			av_frame_unref(decFrame);
		}
	}

	if (!aborted) {
		avcodec_send_frame(encCtx, nullptr);
		flushEnc();
		av_write_trailer(outFmt);
		if (progressCb)
			progressCb(1.0f);
	} else {
		outError = "Cancelled";
	}

	if (sws)
		sws_freeContext(sws);
	av_packet_free(&pkt);
	av_packet_free(&encPkt);
	av_frame_free(&decFrame);
	av_frame_free(&encFrame);
	avio_closep(&outFmt->pb);
	avcodec_free_context(&encCtx);
	avformat_free_context(outFmt);
	avcodec_free_context(&decCtx);
	return !aborted;
}

bool ExportReencode(AVFormatContext *inFmt, const OverlayExportSegment &segment, const std::string &outputPath,
		    double startSec, double endSec, std::atomic<bool> *cancelFlag,
		    const OverlayTrimmer::ProgressCallback &progressCb, std::string &outError)
{
	int vInIdx = -1;
	std::vector<int> aInIndices;
	int audioOrd = 0;

	for (unsigned i = 0; i < inFmt->nb_streams; ++i) {
		auto typ = inFmt->streams[i]->codecpar->codec_type;
		if (typ == AVMEDIA_TYPE_VIDEO && vInIdx < 0) {
			if (IsStreamSelected(static_cast<int>(i), -1, true, false, segment.options,
					     &segment.streamIndicesToKeep))
				vInIdx = static_cast<int>(i);
		} else if (typ == AVMEDIA_TYPE_AUDIO) {
			int curOrd = audioOrd++;
			if (IsStreamSelected(static_cast<int>(i), curOrd, false, true, segment.options,
					     &segment.streamIndicesToKeep))
				aInIndices.push_back(static_cast<int>(i));
		}
	}

	if (vInIdx < 0 && aInIndices.empty()) {
		outError = "FFmpeg: no audio/video streams selected";
		return false;
	}

	AVFormatContext *outFmt = nullptr;
	if (avformat_alloc_output_context2(&outFmt, nullptr, nullptr, outputPath.c_str()) < 0 || !outFmt) {
		outError = "FFmpeg: failed to create output context";
		return false;
	}

	AVCodecContext *vDec = nullptr;
	AVCodecContext *vEnc = nullptr;
	AVStream *vOutSt = nullptr;
	if (vInIdx >= 0) {
		AVStream *inSt = inFmt->streams[vInIdx];
		const AVCodec *dec = avcodec_find_decoder(inSt->codecpar->codec_id);
		if (!dec) {
			avformat_free_context(outFmt);
			outError = "FFmpeg: video decoder not found";
			return false;
		}
		vDec = avcodec_alloc_context3(dec);
		avcodec_parameters_to_context(vDec, inSt->codecpar);
		if (avcodec_open2(vDec, dec, nullptr) < 0) {
			avcodec_free_context(&vDec);
			avformat_free_context(outFmt);
			outError = "FFmpeg: failed to open video decoder";
			return false;
		}

		const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
		if (!enc)
			enc = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!enc) {
			avcodec_free_context(&vDec);
			avformat_free_context(outFmt);
			outError = "FFmpeg: H.264 encoder not found in this build";
			return false;
		}

		int dstW = segment.options.width > 0 ? segment.options.width : vDec->width;
		int dstH = segment.options.height > 0 ? segment.options.height : vDec->height;
		if (segment.options.width > 0 && segment.options.height <= 0 && vDec->width > 0) {
			dstH = static_cast<int>(std::lround(static_cast<double>(dstW) * vDec->height / vDec->width));
		}
		dstW = std::max(2, dstW & ~1);
		dstH = std::max(2, dstH & ~1);

		AVRational srcFps = av_guess_frame_rate(inFmt, inSt, nullptr);
		double targetFps = segment.options.fps > 0.0
					   ? segment.options.fps
					   : (srcFps.num > 0 && srcFps.den > 0 ? av_q2d(srcFps) : 60.0);
		if (targetFps < 1.0 || targetFps > 240.0)
			targetFps = 60.0;
		AVRational fpsRat = av_d2q(targetFps, 10000);

		vEnc = avcodec_alloc_context3(enc);
		vEnc->width = dstW;
		vEnc->height = dstH;
		vEnc->pix_fmt = AV_PIX_FMT_YUV420P;
		vEnc->time_base = av_inv_q(fpsRat);
		vEnc->framerate = fpsRat;
		if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
			vEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		int crf = std::clamp(segment.options.crf, 12, 45);
		AVDictionary *vOpts = nullptr;
		av_dict_set(&vOpts, "preset", "veryfast", 0);
		av_dict_set(&vOpts, "crf", std::to_string(crf).c_str(), 0);
		int ret = avcodec_open2(vEnc, enc, &vOpts);
		av_dict_free(&vOpts);
		if (ret < 0) {
			avcodec_free_context(&vEnc);
			avcodec_free_context(&vDec);
			avformat_free_context(outFmt);
			outError = "FFmpeg: avcodec_open2(video enc) failed: " + AvErrStr(ret);
			return false;
		}

		vOutSt = avformat_new_stream(outFmt, nullptr);
		avcodec_parameters_from_context(vOutSt->codecpar, vEnc);
		vOutSt->time_base = vEnc->time_base;
	}

	struct AudioPipe {
		int inIdx = -1;
		int outIdx = -1;
		AVCodecContext *dec = nullptr;
		AVCodecContext *enc = nullptr;
		AVStream *outSt = nullptr;
		SwrContext *swr = nullptr;
		AVAudioFifo *fifo = nullptr;
		int64_t nextPts = 0;
	};
	std::vector<AudioPipe> aPipes;

	auto cleanupAll = [&]() {
		for (auto &ap : aPipes) {
			if (ap.fifo)
				av_audio_fifo_free(ap.fifo);
			if (ap.swr)
				swr_free(&ap.swr);
			avcodec_free_context(&ap.dec);
			avcodec_free_context(&ap.enc);
		}
		avcodec_free_context(&vEnc);
		avcodec_free_context(&vDec);
		if (outFmt) {
			if (!(outFmt->oformat->flags & AVFMT_NOFILE) && outFmt->pb)
				avio_closep(&outFmt->pb);
			avformat_free_context(outFmt);
		}
	};

	const AVCodec *aacEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
	for (int aIdx : aInIndices) {
		if (!aacEnc) {
			outError = "FFmpeg: AAC encoder not found";
			cleanupAll();
			return false;
		}
		AVStream *inSt = inFmt->streams[aIdx];
		const AVCodec *dec = avcodec_find_decoder(inSt->codecpar->codec_id);
		if (!dec) {
			outError = "FFmpeg: audio decoder not found";
			cleanupAll();
			return false;
		}
		AudioPipe ap;
		ap.inIdx = aIdx;
		ap.dec = avcodec_alloc_context3(dec);
		avcodec_parameters_to_context(ap.dec, inSt->codecpar);
		if (avcodec_open2(ap.dec, dec, nullptr) < 0) {
			avcodec_free_context(&ap.dec);
			outError = "FFmpeg: avcodec_open2(audio) failed";
			cleanupAll();
			return false;
		}

		ap.enc = avcodec_alloc_context3(aacEnc);
		ap.enc->sample_rate = ap.dec->sample_rate > 0 ? ap.dec->sample_rate : 48000;
		AVChannelLayout outLayout{};
		if (ap.dec->ch_layout.nb_channels > 0)
			av_channel_layout_copy(&outLayout, &ap.dec->ch_layout);
		else
			av_channel_layout_default(&outLayout, 2);
		av_channel_layout_copy(&ap.enc->ch_layout, &outLayout);
		av_channel_layout_uninit(&outLayout);
		ap.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
		ap.enc->bit_rate = 192000;
		ap.enc->time_base = AVRational{1, ap.enc->sample_rate};
		if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
			ap.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		if (avcodec_open2(ap.enc, aacEnc, nullptr) < 0) {
			avcodec_free_context(&ap.enc);
			avcodec_free_context(&ap.dec);
			outError = "FFmpeg: avcodec_open2(audio enc) failed";
			cleanupAll();
			return false;
		}

		ap.outSt = avformat_new_stream(outFmt, nullptr);
		avcodec_parameters_from_context(ap.outSt->codecpar, ap.enc);
		ap.outSt->time_base = ap.enc->time_base;
		ap.outIdx = ap.outSt->index;

		AVChannelLayout decLayout{};
		if (ap.dec->ch_layout.nb_channels > 0)
			av_channel_layout_copy(&decLayout, &ap.dec->ch_layout);
		else
			av_channel_layout_default(&decLayout, 2);
		if (swr_alloc_set_opts2(&ap.swr, &ap.enc->ch_layout, ap.enc->sample_fmt, ap.enc->sample_rate,
					&decLayout, ap.dec->sample_fmt, ap.dec->sample_rate, 0, nullptr) < 0 ||
		    swr_init(ap.swr) < 0) {
			av_channel_layout_uninit(&decLayout);
			outError = "FFmpeg: swr_init failed";
			cleanupAll();
			return false;
		}
		av_channel_layout_uninit(&decLayout);

		ap.fifo = av_audio_fifo_alloc(ap.enc->sample_fmt, ap.enc->ch_layout.nb_channels, 4096);
		aPipes.push_back(std::move(ap));
	}

	if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
		int ret = avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0) {
			outError = "FFmpeg: failed to open output file: " + AvErrStr(ret);
			cleanupAll();
			return false;
		}
	}

	AVDictionary *muxOpts = nullptr;
	if (segment.options.format == RebuildOutputFormat::Mp4 ||
	    segment.options.format == RebuildOutputFormat::Mov)
		av_dict_set(&muxOpts, "movflags", "+faststart", 0);
	int ret = avformat_write_header(outFmt, &muxOpts);
	av_dict_free(&muxOpts);
	if (ret < 0) {
		outError = "FFmpeg: avformat_write_header failed: " + AvErrStr(ret);
		cleanupAll();
		return false;
	}

	if (startSec > 0.0) {
		av_seek_frame(inFmt, -1, static_cast<int64_t>(std::llround(startSec * AV_TIME_BASE)),
			      AVSEEK_FLAG_BACKWARD);
	}

	AVPacket *pkt = av_packet_alloc();
	AVPacket *encPkt = av_packet_alloc();
	AVFrame *decFrame = av_frame_alloc();
	AVFrame *vEncFrame = nullptr;
	SwsContext *sws = nullptr;
	if (vEnc) {
		vEncFrame = av_frame_alloc();
		vEncFrame->format = vEnc->pix_fmt;
		vEncFrame->width = vEnc->width;
		vEncFrame->height = vEnc->height;
		av_frame_get_buffer(vEncFrame, 32);
	}

	const double totalSpan = std::max(0.1, endSec - startSec);
	int64_t vFrameCount = 0;
	int64_t lastVEncPts = -1;
	const double vStep = vEnc ? av_q2d(vEnc->time_base) : 1.0 / 60.0;
	double nextVideoOutSec = startSec;
	bool aborted = false;
	bool stopVideo = false;

	auto drainEncoder = [&](AVCodecContext *enc, int outIdx, AVRational outTb) {
		while (avcodec_receive_packet(enc, encPkt) >= 0) {
			av_packet_rescale_ts(encPkt, enc->time_base, outTb);
			encPkt->stream_index = outIdx;
			av_interleaved_write_frame(outFmt, encPkt);
			av_packet_unref(encPkt);
		}
	};

	auto drainAudioFifo = [&](AudioPipe &ap, bool flush) {
		const int frameSize = ap.enc->frame_size > 0 ? ap.enc->frame_size : 1024;
		while (av_audio_fifo_size(ap.fifo) >= frameSize ||
		       (flush && av_audio_fifo_size(ap.fifo) > 0)) {
			int n = std::min(av_audio_fifo_size(ap.fifo), frameSize);
			AVFrame *af = av_frame_alloc();
			af->nb_samples = n;
			av_channel_layout_copy(&af->ch_layout, &ap.enc->ch_layout);
			af->format = ap.enc->sample_fmt;
			af->sample_rate = ap.enc->sample_rate;
			av_frame_get_buffer(af, 0);
			av_audio_fifo_read(ap.fifo, reinterpret_cast<void **>(af->data), n);
			af->pts = ap.nextPts;
			ap.nextPts += n;
			if (avcodec_send_frame(ap.enc, af) >= 0)
				drainEncoder(ap.enc, ap.outIdx, ap.outSt->time_base);
			av_frame_free(&af);
		}
	};

	auto handleVideoFrame = [&](AVFrame *f) {
		AVStream *inSt = inFmt->streams[vInIdx];
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		double sec = overlay::ffmpeg::PtsToSeconds(pts, inSt->time_base);
		if (sec < startSec - 0.01)
			return;
		if (endSec > 0.0 && sec > endSec + 0.01) {
			stopVideo = true;
			return;
		}
		if (segment.options.fps > 0.0 && sec + 0.001 < nextVideoOutSec)
			return;
		while (nextVideoOutSec <= sec + 0.001)
			nextVideoOutSec += vStep;

		sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format),
					   vEnc->width, vEnc->height, vEnc->pix_fmt, SWS_BILINEAR, nullptr,
					   nullptr, nullptr);
		if (!sws)
			return;
		av_frame_make_writable(vEncFrame);
		sws_scale(sws, f->data, f->linesize, 0, f->height, vEncFrame->data, vEncFrame->linesize);
		vEncFrame->pts = (segment.options.fps > 0.0)
					 ? vFrameCount++
					 : av_rescale_q(pts - av_rescale_q(static_cast<int64_t>(startSec * AV_TIME_BASE),
									   AVRational{1, AV_TIME_BASE}, inSt->time_base),
							inSt->time_base, vEnc->time_base);
		if (vEncFrame->pts <= lastVEncPts)
			vEncFrame->pts = lastVEncPts + 1;
		lastVEncPts = vEncFrame->pts;
		if (avcodec_send_frame(vEnc, vEncFrame) >= 0)
			drainEncoder(vEnc, vOutSt->index, vOutSt->time_base);
		if (progressCb)
			progressCb(static_cast<float>(std::clamp((sec - startSec) / totalSpan, 0.0, 0.99)));
	};

	auto handleAudioFrame = [&](AudioPipe &ap, AVFrame *f) {
		AVStream *inSt = inFmt->streams[ap.inIdx];
		int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		double sec = overlay::ffmpeg::PtsToSeconds(pts, inSt->time_base);
		if (sec < startSec - 0.05)
			return;
		if (endSec > 0.0 && sec > endSec + 0.02)
			return;

		int outSamples = av_rescale_rnd(swr_get_delay(ap.swr, ap.dec->sample_rate) + f->nb_samples,
						ap.enc->sample_rate, ap.dec->sample_rate, AV_ROUND_UP);
		uint8_t **convData = nullptr;
		int convLinesize = 0;
		if (av_samples_alloc_array_and_samples(&convData, &convLinesize, ap.enc->ch_layout.nb_channels,
						       outSamples, ap.enc->sample_fmt, 0) >= 0) {
			int got = swr_convert(ap.swr, convData, outSamples,
					      const_cast<const uint8_t **>(f->data), f->nb_samples);
			if (got > 0 &&
			    av_audio_fifo_realloc(ap.fifo, av_audio_fifo_size(ap.fifo) + got) >= 0) {
				av_audio_fifo_write(ap.fifo, reinterpret_cast<void **>(convData), got);
				drainAudioFifo(ap, false);
			}
			av_freep(&convData[0]);
			av_freep(&convData);
		}
	};

	while (av_read_frame(inFmt, pkt) >= 0) {
		if (cancelFlag && cancelFlag->load()) {
			aborted = true;
			av_packet_unref(pkt);
			break;
		}
		if (pkt->stream_index == vInIdx && vDec) {
			if (!stopVideo && avcodec_send_packet(vDec, pkt) >= 0) {
				while (avcodec_receive_frame(vDec, decFrame) >= 0) {
					handleVideoFrame(decFrame);
					av_frame_unref(decFrame);
				}
			}
		} else {
			for (auto &ap : aPipes) {
				if (pkt->stream_index == ap.inIdx) {
					if (avcodec_send_packet(ap.dec, pkt) >= 0) {
						while (avcodec_receive_frame(ap.dec, decFrame) >= 0) {
							handleAudioFrame(ap, decFrame);
							av_frame_unref(decFrame);
						}
					}
					break;
				}
			}
		}
		av_packet_unref(pkt);
		if (stopVideo && endSec > 0.0) {
			// Check if audio already passed endSec too
			bool allPast = true;
			for (const auto &ap : aPipes) {
				double audioSec = static_cast<double>(ap.nextPts) / ap.enc->sample_rate + startSec;
				if (audioSec + 0.1 < endSec) {
					allPast = false;
					break;
				}
			}
			if (allPast)
				break;
		}
	}

	if (!aborted) {
		if (vDec && vEnc) {
			avcodec_send_packet(vDec, nullptr);
			while (avcodec_receive_frame(vDec, decFrame) >= 0) {
				handleVideoFrame(decFrame);
				av_frame_unref(decFrame);
			}
			avcodec_send_frame(vEnc, nullptr);
			drainEncoder(vEnc, vOutSt->index, vOutSt->time_base);
		}
		for (auto &ap : aPipes) {
			avcodec_send_packet(ap.dec, nullptr);
			while (avcodec_receive_frame(ap.dec, decFrame) >= 0) {
				handleAudioFrame(ap, decFrame);
				av_frame_unref(decFrame);
			}
			drainAudioFifo(ap, true);
			avcodec_send_frame(ap.enc, nullptr);
			drainEncoder(ap.enc, ap.outIdx, ap.outSt->time_base);
		}
		av_write_trailer(outFmt);
		if (progressCb)
			progressCb(1.0f);
	} else {
		outError = "Cancelled";
	}

	if (sws)
		sws_freeContext(sws);
	av_packet_free(&pkt);
	av_packet_free(&encPkt);
	av_frame_free(&decFrame);
	av_frame_free(&vEncFrame);
	cleanupAll();
	return !aborted;
}

bool ConcatSegmentsCopy(const std::vector<std::string> &tempFiles, const std::string &outputPath,
			std::atomic<bool> *cancelFlag, const OverlayTrimmer::ProgressCallback &progressCb,
			std::string &outError)
{
	if (tempFiles.empty()) {
		outError = "Merge concat: no inputs";
		return false;
	}
	if (tempFiles.size() == 1) {
		std::error_code ec;
		std::filesystem::copy_file(tempFiles[0], outputPath,
					   std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			outError = "Merge concat: failed to copy single segment: " + ec.message();
			return false;
		}
		if (progressCb)
			progressCb(1.0f);
		return true;
	}

	AVFormatContext *firstIn = nullptr;
	if (avformat_open_input(&firstIn, tempFiles[0].c_str(), nullptr, nullptr) < 0 || !firstIn) {
		outError = "FFmpeg: merge open first segment failed";
		return false;
	}
	if (avformat_find_stream_info(firstIn, nullptr) < 0) {
		avformat_close_input(&firstIn);
		outError = "FFmpeg: merge first segment info failed";
		return false;
	}

	AVFormatContext *outFmt = nullptr;
	if (avformat_alloc_output_context2(&outFmt, nullptr, nullptr, outputPath.c_str()) < 0 || !outFmt) {
		avformat_close_input(&firstIn);
		outError = "FFmpeg: failed to create merge concat output";
		return false;
	}

	for (unsigned i = 0; i < firstIn->nb_streams; ++i) {
		AVStream *inSt = firstIn->streams[i];
		AVStream *outSt = avformat_new_stream(outFmt, nullptr);
		if (!outSt || avcodec_parameters_copy(outSt->codecpar, inSt->codecpar) < 0) {
			avformat_close_input(&firstIn);
			avformat_free_context(outFmt);
			outError = "FFmpeg: avcodec_parameters_copy(merge concat) failed";
			return false;
		}
		outSt->codecpar->codec_tag = 0;
		outSt->time_base = inSt->time_base;
	}
	avformat_close_input(&firstIn);

	if (outFmt->nb_streams == 0) {
		avformat_free_context(outFmt);
		outError = "FFmpeg: merge concat: first segment has no A/V streams";
		return false;
	}

	if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
			avformat_free_context(outFmt);
			outError = "FFmpeg: open merge concat output failed";
			return false;
		}
	}

	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "movflags", "+faststart", 0);
	int ret = avformat_write_header(outFmt, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		avio_closep(&outFmt->pb);
		avformat_free_context(outFmt);
		outError = "FFmpeg: merge concat write_header failed";
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	std::vector<int64_t> streamOffsetUs(outFmt->nb_streams, 0);
	std::vector<int64_t> lastOutDts(outFmt->nb_streams, AV_NOPTS_VALUE);

	for (size_t fi = 0; fi < tempFiles.size(); ++fi) {
		if (cancelFlag && cancelFlag->load()) {
			outError = "Cancelled";
			av_packet_free(&pkt);
			avio_closep(&outFmt->pb);
			avformat_free_context(outFmt);
			return false;
		}

		AVFormatContext *segIn = nullptr;
		if (avformat_open_input(&segIn, tempFiles[fi].c_str(), nullptr, nullptr) < 0 || !segIn) {
			outError = "FFmpeg: merge open segment failed";
			av_packet_free(&pkt);
			avio_closep(&outFmt->pb);
			avformat_free_context(outFmt);
			return false;
		}
		avformat_find_stream_info(segIn, nullptr);

		std::vector<int64_t> maxSegEndUs(outFmt->nb_streams, 0);
		while (av_read_frame(segIn, pkt) >= 0) {
			int idx = pkt->stream_index;
			if (idx < 0 || static_cast<unsigned>(idx) >= outFmt->nb_streams) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *inSt = segIn->streams[idx];
			AVStream *outSt = outFmt->streams[idx];

			int64_t endTs = (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts) + pkt->duration;
			if (endTs > 0) {
				int64_t endUs = av_rescale_q(endTs, inSt->time_base, AVRational{1, AV_TIME_BASE});
				if (endUs > maxSegEndUs[idx])
					maxSegEndUs[idx] = endUs;
			}

			int64_t offTs = av_rescale_q(streamOffsetUs[idx], AVRational{1, AV_TIME_BASE},
						     outSt->time_base);
			av_packet_rescale_ts(pkt, inSt->time_base, outSt->time_base);
			if (pkt->pts != AV_NOPTS_VALUE)
				pkt->pts += offTs;
			if (pkt->dts != AV_NOPTS_VALUE)
				pkt->dts += offTs;
			if (pkt->dts != AV_NOPTS_VALUE) {
				if (lastOutDts[idx] != AV_NOPTS_VALUE && pkt->dts <= lastOutDts[idx])
					pkt->dts = lastOutDts[idx] + 1;
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
					pkt->pts = pkt->dts;
				lastOutDts[idx] = pkt->dts;
			}
			pkt->pos = -1;
			av_interleaved_write_frame(outFmt, pkt);
			av_packet_unref(pkt);
		}

		int64_t segAdvanceUs = 0;
		for (int64_t v : maxSegEndUs)
			segAdvanceUs = std::max(segAdvanceUs, v);
		if (segAdvanceUs <= 0 && segIn->duration > 0)
			segAdvanceUs = segIn->duration;
		for (unsigned idx = 0; idx < outFmt->nb_streams; ++idx)
			streamOffsetUs[idx] += segAdvanceUs;

		avformat_close_input(&segIn);
		if (progressCb)
			progressCb(0.65f + 0.35f * static_cast<float>(fi + 1) / tempFiles.size());
	}

	av_packet_free(&pkt);
	av_write_trailer(outFmt);
	if (!(outFmt->oformat->flags & AVFMT_NOFILE) && outFmt->pb)
		avio_closep(&outFmt->pb);
	avformat_free_context(outFmt);
	return true;
}

} // namespace

bool OverlayTrimmer::ProbePlaybackRange(const std::string &filePath, PlaybackProbeResult &out,
					  bool fullPacketScan)
{
	out = PlaybackProbeResult{};
	if (filePath.empty()) {
		out.error = "Invalid input path";
		return false;
	}

	std::error_code ec;
	if (std::filesystem::exists(filePath, ec))
		out.sizeBytes = static_cast<uint64_t>(std::filesystem::file_size(filePath, ec));

	AVFormatContext *fmt = nullptr;
	int ret = avformat_open_input(&fmt, filePath.c_str(), nullptr, nullptr);
	if (ret < 0 || !fmt) {
		out.error = AvErrStr(ret);
		return false;
	}
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		out.error = "avformat_find_stream_info failed";
		avformat_close_input(&fmt);
		return false;
	}

	out.containerDuration =
		fmt->duration != AV_NOPTS_VALUE ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
	out.refVideoStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

	if (!fullPacketScan && out.containerDuration > 0.0) {
		out.startSec = 0.0;
		out.endSec = out.containerDuration;
		out.spanSec = out.containerDuration;
		out.valid = true;
		avformat_close_input(&fmt);
		return true;
	}

	int refIdx = out.refVideoStream >= 0 ? out.refVideoStream : 0;
	if (static_cast<unsigned>(refIdx) >= fmt->nb_streams) {
		avformat_close_input(&fmt);
		out.error = "No streams found";
		return false;
	}

	AVStream *refSt = fmt->streams[refIdx];
	AVPacket *pkt = av_packet_alloc();
	double minSec = std::numeric_limits<double>::max();
	double maxSec = 0.0;
	bool sawAny = false;

	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == refIdx) {
			++out.videoPackets;
			int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
			if (ts != AV_NOPTS_VALUE) {
				double s = overlay::ffmpeg::PtsToSeconds(ts, refSt->time_base);
				double d = pkt->duration > 0
						   ? overlay::ffmpeg::PtsToSeconds(pkt->duration, refSt->time_base)
						   : 0.0;
				minSec = std::min(minSec, s);
				maxSec = std::max(maxSec, s + d);
				sawAny = true;
			}
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	avformat_close_input(&fmt);

	if (!sawAny) {
		out.startSec = 0.0;
		out.endSec = std::max(0.0, out.containerDuration);
	} else {
		out.startSec = std::max(0.0, minSec);
		out.endSec = std::max(out.startSec, maxSec);
	}
	out.spanSec = std::max(0.0, out.endSec - out.startSec);
	out.valid = out.spanSec > 0.0 || out.containerDuration > 0.0;
	return out.valid;
}

bool OverlayTrimmer::ExportSegment(const OverlayExportSegment &segment, const std::string &outputPath,
				   std::atomic<bool> *cancelFlag, const ProgressCallback &progressCb,
				   std::string &outError)
{
	if (segment.inputPath.empty() || outputPath.empty()) {
		outError = "Invalid input/output path";
		return false;
	}
	std::error_code ec;
	if (!std::filesystem::exists(segment.inputPath, ec)) {
		outError = "Input file not found: " + segment.inputPath;
		return false;
	}

	AVFormatContext *inFmt = nullptr;
	int ret = avformat_open_input(&inFmt, segment.inputPath.c_str(), nullptr, nullptr);
	if (ret < 0 || !inFmt) {
		outError = "FFmpeg: failed to open input: " + AvErrStr(ret);
		return false;
	}
	if (avformat_find_stream_info(inFmt, nullptr) < 0) {
		avformat_close_input(&inFmt);
		outError = "FFmpeg: failed to read stream info";
		return false;
	}

	double dur = inFmt->duration != AV_NOPTS_VALUE
			     ? static_cast<double>(inFmt->duration) / AV_TIME_BASE
			     : 0.0;
	double startSec = std::max(0.0, segment.startSeconds);
	double endSec = segment.endSeconds > startSec ? segment.endSeconds : dur;

	bool ok = false;
	if (segment.options.format == RebuildOutputFormat::Gif) {
		ok = ExportGif(inFmt, segment, outputPath, startSec, endSec, cancelFlag, progressCb, outError);
	} else if (segment.options.mode == RebuildMode::Copy) {
		ok = ExportStreamCopy(inFmt, segment, outputPath, startSec, endSec, cancelFlag, progressCb,
				      outError);
	} else {
		ok = ExportReencode(inFmt, segment, outputPath, startSec, endSec, cancelFlag, progressCb,
				    outError);
	}

	avformat_close_input(&inFmt);
	return ok;
}

bool OverlayTrimmer::ExportSegmentsMerged(const std::vector<OverlayExportSegment> &segments,
					    const std::string &outputPath,
					    const OverlayExportOptions &finalOptions,
					    std::atomic<bool> *cancelFlag, const ProgressCallback &progressCb,
					    std::string &outError)
{
	if (segments.empty()) {
		outError = "Merge export: no segments";
		return false;
	}
	if (outputPath.empty()) {
		outError = "Output path is empty";
		return false;
	}

	std::error_code ec;
	std::filesystem::path tmpDir = std::filesystem::temp_directory_path(ec);
	if (ec || tmpDir.empty())
		tmpDir = "/tmp";

	std::vector<std::string> tempFiles;
	auto cleanupTemps = [&]() {
		for (const auto &f : tempFiles) {
			std::error_code rmEc;
			std::filesystem::remove(f, rmEc);
		}
	};

	// Normalize intermediate segments into consistent H.264/AAC .mp4 files so concat always matches
	for (size_t i = 0; i < segments.size(); ++i) {
		if (cancelFlag && cancelFlag->load()) {
			cleanupTemps();
			outError = "Cancelled";
			return false;
		}
		char nameBuf[128];
		std::snprintf(nameBuf, sizeof(nameBuf), "ntchffs_merge_%zu_%d.mp4", i,
			      static_cast<int>(std::rand() & 0xFFFF));
		std::string tmpPath = (tmpDir / nameBuf).string();
		tempFiles.push_back(tmpPath);

		OverlayExportSegment norm = segments[i];
		norm.options = finalOptions;
		norm.options.mode = RebuildMode::Reencode;
		norm.options.format = RebuildOutputFormat::Mp4;

		auto segCb = [&](float p) {
			if (progressCb) {
				float base = static_cast<float>(i) / segments.size();
				float span = 0.65f / segments.size();
				progressCb(base * 0.65f + p * span);
			}
		};

		if (!ExportSegment(norm, tmpPath, cancelFlag, segCb, outError)) {
			outError = "Merge segment " + std::to_string(i + 1) + " failed: " + outError;
			cleanupTemps();
			return false;
		}
	}

	bool ok = false;
	if (finalOptions.format == RebuildOutputFormat::Gif) {
		char concatBuf[128];
		std::snprintf(concatBuf, sizeof(concatBuf), "ntchffs_merge_concat_%d.mp4",
			      static_cast<int>(std::rand() & 0xFFFF));
		std::string concatTmp = (tmpDir / concatBuf).string();
		tempFiles.push_back(concatTmp);
		if (ConcatSegmentsCopy(tempFiles.size() > 1 ? std::vector<std::string>(tempFiles.begin(),
										     tempFiles.end() - 1)
							    : tempFiles,
				       concatTmp, cancelFlag, nullptr, outError)) {
			OverlayExportSegment finalGifSeg;
			finalGifSeg.inputPath = concatTmp;
			finalGifSeg.options = finalOptions;
			ok = ExportSegment(finalGifSeg, outputPath, cancelFlag, progressCb, outError);
		}
	} else {
		ok = ConcatSegmentsCopy(tempFiles, outputPath, cancelFlag, progressCb, outError);
	}

	cleanupTemps();
	return ok;
}

bool OverlayTrimmer::TrimKeepLastSeconds(const std::string &filePath, double keepSeconds, double epsilonSec,
					 std::string &outTempPath, PlaybackProbeResult *preProbeOut,
					 double *outPlannedTrimStart, std::string &outError)
{
	outTempPath.clear();
	if (keepSeconds <= 0.0) {
		outError = "Invalid keep duration";
		return false;
	}

	PlaybackProbeResult probe;
	if (!ProbePlaybackRange(filePath, probe, true)) {
		outError = probe.error.empty() ? "Probe failed" : probe.error;
		return false;
	}
	if (preProbeOut)
		*preProbeOut = probe;

	if (probe.spanSec <= keepSeconds + epsilonSec) {
		if (outPlannedTrimStart)
			*outPlannedTrimStart = probe.startSec;
		return true; // No-op; caller sees empty outTempPath
	}

	double trimStart = std::max(0.0, probe.endSec - keepSeconds);
	if (outPlannedTrimStart)
		*outPlannedTrimStart = trimStart;

	std::filesystem::path src(filePath);
	std::string ext = src.extension().string();
	if (ext.empty())
		ext = ".mp4";
	std::string tmpPath = src.parent_path().empty()
				      ? (src.stem().string() + ".trim_tmp" + ext)
				      : (src.parent_path() / (src.stem().string() + ".trim_tmp" + ext)).string();

	OverlayExportSegment seg;
	seg.inputPath = filePath;
	seg.startSeconds = trimStart;
	seg.endSeconds = probe.endSec;
	seg.options.mode = RebuildMode::Copy;
	if (ext == ".mov" || ext == ".MOV")
		seg.options.format = RebuildOutputFormat::Mov;
	else
		seg.options.format = RebuildOutputFormat::Mp4;

	if (!ExportSegment(seg, tmpPath, nullptr, nullptr, outError)) {
		std::error_code ec;
		std::filesystem::remove(tmpPath, ec);
		return false;
	}

	outTempPath = std::move(tmpPath);
	return true;
}
