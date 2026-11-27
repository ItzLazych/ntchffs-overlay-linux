#include "overlay-video-player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "overlay-ffmpeg-time-utils.h"
#include "overlay-ffmpeg-video-decode.h"
#include "overlay-mp4-box-reader.h"
#include "plugin-support.h"

OverlayVideoPlayer::~OverlayVideoPlayer()
{
	Close();
}

bool OverlayVideoPlayer::Open(const std::string &filePath, std::function<void()> onFrameReady)
{
	Close();
	if (filePath.empty())
		return false;

	AVFormatContext *fmt = nullptr;
	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "probesize", "5000000", 0);
	av_dict_set(&opts, "analyzeduration", "2000000", 0);
	int ret = avformat_open_input(&fmt, filePath.c_str(), nullptr, &opts);
	av_dict_free(&opts);
	if (ret < 0 || !fmt) {
		char err[AV_ERROR_MAX_STRING_SIZE]{};
		av_strerror(ret, err, sizeof(err));
		obs_log(LOG_WARNING, "OverlayVideoPlayer: avformat_open_input failed: %s", err);
		return false;
	}

	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	AVStream *vSt = fmt->streams[vIdx];
	videoStreamIndex_ = vIdx;
	videoWidth_ = vSt->codecpar->width;
	videoHeight_ = vSt->codecpar->height;
	AVRational fps = av_guess_frame_rate(fmt, vSt, nullptr);
	videoFps_ = (fps.num > 0 && fps.den > 0) ? av_q2d(fps) : 60.0;
	if (videoFps_ < 1.0 || videoFps_ > 240.0)
		videoFps_ = 60.0;

	durationSec_ = fmt->duration != AV_NOPTS_VALUE
			       ? static_cast<double>(fmt->duration) / AV_TIME_BASE
			       : 0.0;

	// Discover audio streams and enrich names from MP4 track names if available
	std::vector<std::string> mp4Names = overlay_mp4::ReadMp4AudioTrackNames(filePath);
	audioStreams_.clear();
	int ord = 0;
	for (unsigned i = 0; i < fmt->nb_streams; ++i) {
		AVStream *st = fmt->streams[i];
		if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		AudioStreamInfo info;
		info.streamIndex = static_cast<int>(i);
		info.audioOrdinal = ord;
		info.channels = st->codecpar->ch_layout.nb_channels > 0
					? st->codecpar->ch_layout.nb_channels
					: 2;
		info.sampleRate = st->codecpar->sample_rate > 0 ? st->codecpar->sample_rate : 48000;

		if (AVDictionaryEntry *e = av_dict_get(st->metadata, "language", nullptr, 0))
			info.language = e->value ? e->value : "";
		if (ord >= 0 && static_cast<size_t>(ord) < mp4Names.size() && !mp4Names[ord].empty()) {
			info.name = mp4Names[ord];
		} else if (AVDictionaryEntry *t = av_dict_get(st->metadata, "title", nullptr, 0)) {
			if (t->value && *t->value)
				info.name = t->value;
		} else if (AVDictionaryEntry *h = av_dict_get(st->metadata, "handler_name", nullptr, 0)) {
			if (h->value && *h->value && std::string(h->value) != "SoundHandler" &&
			    std::string(h->value) != "AudioHandler")
				info.name = h->value;
		}
		if (info.name.empty())
			info.name = "Audio " + std::to_string(ord + 1);
		audioStreams_.push_back(std::move(info));
		++ord;
	}
	avformat_close_input(&fmt);

	filePath_ = filePath;
	onFrameReady_ = std::move(onFrameReady);
	activeAudioStream_ = audioStreams_.empty() ? -1 : audioStreams_[0].streamIndex;
	positionSec_ = 0.0;
	seekTargetSec_ = 0.0;
	seekPending_ = false;
	stepDelta_ = 0;
	stopRequested_ = false;
	playing_ = false;
	open_ = true;

	worker_ = std::thread(&OverlayVideoPlayer::PlayerLoop, this);
	return true;
}

void OverlayVideoPlayer::Close()
{
	stopRequested_ = true;
	if (worker_.joinable())
		worker_.join();
	open_ = false;
	playing_ = false;
	filePath_.clear();
	audioStreams_.clear();
	std::lock_guard<std::mutex> lock(frameMutex_);
	currentFrame_ = QImage();
}

void OverlayVideoPlayer::Play()
{
	if (open_)
		playing_ = true;
}

void OverlayVideoPlayer::Pause()
{
	playing_ = false;
}

void OverlayVideoPlayer::TogglePlayPause()
{
	if (open_)
		playing_ = !playing_.load();
}

void OverlayVideoPlayer::Seek(double seconds)
{
	if (!open_)
		return;
	seekTargetSec_ = std::clamp(seconds, 0.0, std::max(0.0, durationSec_));
	seekPending_ = true;
}

void OverlayVideoPlayer::StepFrame(int deltaFrames)
{
	if (!open_)
		return;
	playing_ = false;
	if (deltaFrames < 0) {
		double backSec = std::max(0.0, positionSec_.load() + deltaFrames / std::max(1.0, videoFps_));
		seekTargetSec_ = backSec;
		seekPending_ = true;
	} else if (deltaFrames > 0) {
		stepDelta_ += deltaFrames;
	}
}

void OverlayVideoPlayer::SetVolume(float volume01)
{
	volume_ = std::clamp(volume01, 0.0f, 1.0f);
}

void OverlayVideoPlayer::SetMuted(bool muted)
{
	muted_ = muted;
}

void OverlayVideoPlayer::SelectAudioStream(int streamIndex)
{
	activeAudioStream_ = streamIndex;
}

bool OverlayVideoPlayer::CopyCurrentFrame(QImage &dst, int *outW, int *outH) const
{
	std::lock_guard<std::mutex> lock(frameMutex_);
	if (currentFrame_.isNull())
		return false;
	dst = currentFrame_;
	if (outW)
		*outW = currentFrame_.width();
	if (outH)
		*outH = currentFrame_.height();
	return true;
}

void OverlayVideoPlayer::PlayerLoop()
{
	AVFormatContext *fmt = nullptr;
	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "probesize", "5000000", 0);
	av_dict_set(&opts, "analyzeduration", "2000000", 0);
	if (avformat_open_input(&fmt, filePath_.c_str(), nullptr, &opts) < 0 || !fmt) {
		av_dict_free(&opts);
		open_ = false;
		return;
	}
	av_dict_free(&opts);
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		open_ = false;
		return;
	}

	AVStream *vSt = fmt->streams[videoStreamIndex_];
	const AVCodec *vCodec = avcodec_find_decoder(vSt->codecpar->codec_id);
	if (!vCodec) {
		obs_log(LOG_WARNING, "[OverlayVideoPlayer] No decoder for video codec_id=%d (path: %s)",
			static_cast<int>(vSt->codecpar->codec_id), filePath_.c_str());
		avformat_close_input(&fmt);
		open_ = false;
		return;
	}

	AVCodecContext *vDec = avcodec_alloc_context3(vCodec);
	overlay::ffmpeg::VideoHwDecodeState hwState;
	if (!vDec || avcodec_parameters_to_context(vDec, vSt->codecpar) < 0) {
		avcodec_free_context(&vDec);
		avformat_close_input(&fmt);
		open_ = false;
		return;
	}
	hwState.TryInitForDecoder(vDec, vCodec);
	if (avcodec_open2(vDec, vCodec, nullptr) < 0) {
		hwState.Reset();
		avcodec_free_context(&vDec);
		vDec = avcodec_alloc_context3(vCodec);
		avcodec_parameters_to_context(vDec, vSt->codecpar);
		if (avcodec_open2(vDec, vCodec, nullptr) < 0) {
			obs_log(LOG_WARNING, "[OverlayVideoPlayer] avcodec_open2 failed for video decoder %s",
				vCodec->name);
			avcodec_free_context(&vDec);
			avformat_close_input(&fmt);
			open_ = false;
			return;
		}
	}

	// Audio setup
	int curAIdx = -1;
	AVCodecContext *aDec = nullptr;
	SwrContext *swr = nullptr;
	pa_simple *pa = nullptr;

	auto openAudioForStream = [&](int newAIdx) {
		if (swr)
			swr_free(&swr);
		avcodec_free_context(&aDec);
		curAIdx = -1;
		if (newAIdx < 0 || static_cast<unsigned>(newAIdx) >= fmt->nb_streams)
			return;
		AVStream *aSt = fmt->streams[newAIdx];
		if (aSt->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			return;
		const AVCodec *ac = avcodec_find_decoder(aSt->codecpar->codec_id);
		if (!ac)
			return;
		aDec = avcodec_alloc_context3(ac);
		if (!aDec || avcodec_parameters_to_context(aDec, aSt->codecpar) < 0 ||
		    avcodec_open2(aDec, ac, nullptr) < 0) {
			avcodec_free_context(&aDec);
			return;
		}
		AVChannelLayout inLayout{};
		if (aDec->ch_layout.nb_channels > 0)
			av_channel_layout_copy(&inLayout, &aDec->ch_layout);
		else
			av_channel_layout_default(&inLayout, 2);
		AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
		if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, 48000, &inLayout,
					aDec->sample_fmt, aDec->sample_rate > 0 ? aDec->sample_rate : 48000,
					0, nullptr) < 0 ||
		    swr_init(swr) < 0) {
			av_channel_layout_uninit(&inLayout);
			swr_free(&swr);
			avcodec_free_context(&aDec);
			return;
		}
		av_channel_layout_uninit(&inLayout);

		if (!pa) {
			static const pa_sample_spec ss{PA_SAMPLE_FLOAT32LE, 48000, 2};
			int paErr = 0;
			pa = pa_simple_new(nullptr, "ntchffs-overlay", PA_STREAM_PLAYBACK, nullptr,
					   "Video Preview", &ss, nullptr, nullptr, &paErr);
		}
		curAIdx = newAIdx;
	};

	openAudioForStream(activeAudioStream_.load());

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *seekFrame = av_frame_alloc();
	AVFrame *swFrame = av_frame_alloc();
	SwsContext *sws = nullptr;

	const double fmtStartSec = (fmt->start_time != AV_NOPTS_VALUE)
					   ? static_cast<double>(fmt->start_time) / AV_TIME_BASE
					   : 0.0;

	auto frameSeconds = [&](const AVFrame *f) -> double {
		int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
		double s = overlay::ffmpeg::PtsToSeconds(ts, vSt->time_base) - fmtStartSec;
		return s < 0.0 ? 0.0 : s;
	};

	auto clockBase = std::chrono::steady_clock::now();
	double ptsBase = 0.0;
	bool haveClockBase = false;
	bool eofReached = false;

	auto uploadVideoFrame = [&](AVFrame *src) {
		AVFrame *usable = nullptr;
		if (!hwState.TransferToSwIfNeeded(src, swFrame, &usable) || !usable)
			return;
		if (usable->width <= 0 || usable->height <= 0)
			return;
		sws = sws_getCachedContext(sws, usable->width, usable->height,
					   static_cast<AVPixelFormat>(usable->format), usable->width,
					   usable->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
					   nullptr);
		if (!sws)
			return;

		QImage img(usable->width, usable->height, QImage::Format_ARGB32_Premultiplied);
		uint8_t *dstData[4] = {img.bits(), nullptr, nullptr, nullptr};
		int dstLinesize[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
		sws_scale(sws, usable->data, usable->linesize, 0, usable->height, dstData, dstLinesize);

		{
			std::lock_guard<std::mutex> lock(frameMutex_);
			currentFrame_ = std::move(img);
		}
		if (onFrameReady_)
			onFrameReady_();
	};

	auto decodeFirstFrame = [&]() {
		while (!stopRequested_ && !seekPending_) {
			if (av_read_frame(fmt, pkt) < 0)
				break;
			bool gotVideo = false;
			if (pkt->stream_index == videoStreamIndex_) {
				if (avcodec_send_packet(vDec, pkt) >= 0) {
					if (avcodec_receive_frame(vDec, frame) >= 0) {
						double sec = frameSeconds(frame);
						positionSec_ = sec;
						ptsBase = sec;
						clockBase = std::chrono::steady_clock::now();
						haveClockBase = true;
						uploadVideoFrame(frame);
						av_frame_unref(frame);
						gotVideo = true;
					}
				}
			}
			av_packet_unref(pkt);
			if (gotVideo)
				break;
		}
	};

	decodeFirstFrame();

	while (!stopRequested_) {
		int desiredA = activeAudioStream_.load();
		if (desiredA != curAIdx)
			openAudioForStream(desiredA);

		if (seekPending_.exchange(false)) {
			double tgt = seekTargetSec_.load();
			int64_t seekUs = (fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0) +
					 static_cast<int64_t>(std::llround(tgt * AV_TIME_BASE));
			av_seek_frame(fmt, -1, seekUs, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(vDec);
			if (aDec)
				avcodec_flush_buffers(aDec);
			if (swr)
				swr_convert(swr, nullptr, 0, nullptr, 0);
			if (pa) {
				int err = 0;
				pa_simple_flush(pa, &err);
			}
			haveClockBase = false;
			eofReached = false;

			// Decode silently from previous keyframe to tgt so paused/click seeks jump
			// straight to the target frame without fast-forwarding intermediate frames.
			av_frame_unref(seekFrame);
			double bestSec = 0.0;
			bool haveBestFrame = false;
			bool found = false;
			bool hitEof = false;
			const double frameTol = 0.5 / std::max(1.0, videoFps_);

			auto collectSeekFrames = [&]() {
				while (avcodec_receive_frame(vDec, frame) >= 0) {
					double sec = frameSeconds(frame);
					av_frame_unref(seekFrame);
					av_frame_move_ref(seekFrame, frame);
					bestSec = sec;
					haveBestFrame = true;
					if (sec + frameTol >= tgt) {
						found = true;
						break;
					}
				}
			};

			while (!stopRequested_ && !seekPending_ && !found) {
				if (av_read_frame(fmt, pkt) < 0) {
					hitEof = true;
					break;
				}
				if (pkt->stream_index == videoStreamIndex_) {
					if (avcodec_send_packet(vDec, pkt) >= 0)
						collectSeekFrames();
				}
				av_packet_unref(pkt);
			}
			if (!stopRequested_ && !seekPending_ && !found && hitEof) {
				if (avcodec_send_packet(vDec, nullptr) >= 0)
					collectSeekFrames();
			}
			if (!stopRequested_ && !seekPending_ && haveBestFrame) {
				positionSec_ = bestSec;
				ptsBase = bestSec;
				clockBase = std::chrono::steady_clock::now();
				haveClockBase = true;
				uploadVideoFrame(seekFrame);
			}
			av_frame_unref(seekFrame);
			continue;
		}

		bool singleStep = stepDelta_.load() > 0;
		if (!playing_ && !singleStep) {
			haveClockBase = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
			continue;
		}

		if (eofReached) {
			// Loop back to 0 when user clicks Play after EOF or keep paused at end
			playing_ = false;
			seekTargetSec_ = 0.0;
			seekPending_ = true;
			eofReached = false;
			continue;
		}

		if (av_read_frame(fmt, pkt) < 0) {
			eofReached = true;
			continue;
		}

		if (pkt->stream_index == videoStreamIndex_) {
			if (avcodec_send_packet(vDec, pkt) >= 0) {
				while (avcodec_receive_frame(vDec, frame) >= 0) {
					double sec = frameSeconds(frame);
					if (!singleStep) {
						if (!haveClockBase) {
							ptsBase = sec;
							clockBase = std::chrono::steady_clock::now();
							haveClockBase = true;
						} else {
							double targetElapsed = sec - ptsBase;
							auto deadline = clockBase +
									std::chrono::duration<double>(targetElapsed);
							while (!stopRequested_ && !seekPending_ && playing_) {
								auto now = std::chrono::steady_clock::now();
								if (now >= deadline)
									break;
								auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
									deadline - now);
								if (rem.count() > 2)
									std::this_thread::sleep_for(
										std::min(rem, std::chrono::milliseconds(10)));
								else
									break;
							}
						}
					} else {
						--stepDelta_;
					}
					positionSec_ = sec;
					uploadVideoFrame(frame);
					av_frame_unref(frame);
					if (stopRequested_ || seekPending_)
						break;
				}
			}
		} else if (pkt->stream_index == curAIdx && aDec && swr && pa && !singleStep) {
			if (avcodec_send_packet(aDec, pkt) >= 0) {
				while (avcodec_receive_frame(aDec, frame) >= 0) {
					int maxOut = av_rescale_rnd(swr_get_delay(swr, aDec->sample_rate) +
									    frame->nb_samples,
								    48000, aDec->sample_rate, AV_ROUND_UP);
					std::vector<float> buf(static_cast<size_t>(std::max(1, maxOut)) * 2);
					uint8_t *outPtr[1] = {reinterpret_cast<uint8_t *>(buf.data())};
					int got = swr_convert(swr, outPtr, maxOut,
							      const_cast<const uint8_t **>(frame->data),
							      frame->nb_samples);
					if (got > 0) {
						float gain = muted_.load() ? 0.0f : volume_.load();
						for (int i = 0; i < got * 2; ++i)
							buf[i] *= gain;
						int err = 0;
						pa_simple_write(pa, buf.data(),
								static_cast<size_t>(got) * 2 * sizeof(float), &err);
					}
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (pa) {
		int err = 0;
		pa_simple_drain(pa, &err);
		pa_simple_free(pa);
	}
	if (sws)
		sws_freeContext(sws);
	if (swr)
		swr_free(&swr);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_frame_free(&seekFrame);
	av_frame_free(&swFrame);
	avcodec_free_context(&aDec);
	hwState.Reset();
	avcodec_free_context(&vDec);
	avformat_close_input(&fmt);
}
