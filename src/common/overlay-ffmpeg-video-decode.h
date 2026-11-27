#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <cstring>

namespace overlay::ffmpeg {

struct VideoHwDecodeState {
	AVBufferRef *hwDeviceCtx = nullptr;
	AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
	bool active = false;

	~VideoHwDecodeState() { Reset(); }

	void Reset()
	{
		if (hwDeviceCtx) {
			av_buffer_unref(&hwDeviceCtx);
			hwDeviceCtx = nullptr;
		}
		hwPixFmt = AV_PIX_FMT_NONE;
		active = false;
	}

	static enum AVPixelFormat GetHwFormatCallback(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
	{
		auto *self = static_cast<VideoHwDecodeState *>(ctx ? ctx->opaque : nullptr);
		if (!self)
			return AV_PIX_FMT_NONE;
		for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == self->hwPixFmt)
				return *p;
		}
		return AV_PIX_FMT_NONE;
	}

	bool TryInitForDecoder(AVCodecContext *ctx, const AVCodec *codec)
	{
		Reset();
		if (!ctx || !codec)
			return false;

		// Prefer VAAPI or VDPAU or CUDA/Vulkan if available, else software decode
		static constexpr AVHWDeviceType kPreferred[] = {
			AV_HWDEVICE_TYPE_VAAPI,
			AV_HWDEVICE_TYPE_VDPAU,
			AV_HWDEVICE_TYPE_VULKAN,
		};

		for (AVHWDeviceType hwType : kPreferred) {
			for (int i = 0;; ++i) {
				const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
				if (!cfg)
					break;
				if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
				    cfg->device_type == hwType) {
					AVBufferRef *devCtx = nullptr;
					if (av_hwdevice_ctx_create(&devCtx, hwType, nullptr, nullptr, 0) >= 0) {
						hwDeviceCtx = devCtx;
						hwPixFmt = cfg->pix_fmt;
						ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
						ctx->opaque = this;
						ctx->get_format = &VideoHwDecodeState::GetHwFormatCallback;
						active = true;
						return true;
					}
				}
			}
		}
		return false;
	}

	bool TransferToSwIfNeeded(AVFrame *srcFrame, AVFrame *swFrame, AVFrame **outFrame) const
	{
		if (!srcFrame || !outFrame)
			return false;
		if (active && srcFrame->format == hwPixFmt && swFrame) {
			av_frame_unref(swFrame);
			if (av_hwframe_transfer_data(swFrame, srcFrame, 0) >= 0) {
				swFrame->pts = srcFrame->pts;
				swFrame->best_effort_timestamp = srcFrame->best_effort_timestamp;
				*outFrame = swFrame;
				return true;
			}
			return false;
		}
		*outFrame = srcFrame;
		return true;
	}
};

} // namespace overlay::ffmpeg
