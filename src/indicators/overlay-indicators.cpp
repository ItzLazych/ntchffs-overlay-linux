#include "overlay-indicators.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <pulse/error.h>
#include <pulse/simple.h>

#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

#include "overlay-wayland-layer.h"
#include "overlay-window-manager.h"
#include "plugin-support.h"

namespace {

void PlayAudioFileAsync(const std::string &path)
{
	if (path.empty())
		return;
	std::thread([path]() {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
			return;

		AVFormatContext *fmt = nullptr;
		if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 || !fmt)
			return;
		if (avformat_find_stream_info(fmt, nullptr) < 0) {
			avformat_close_input(&fmt);
			return;
		}
		int aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (aIdx < 0) {
			avformat_close_input(&fmt);
			return;
		}
		AVStream *st = fmt->streams[aIdx];
		const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec) {
			avformat_close_input(&fmt);
			return;
		}
		AVCodecContext *dec = avcodec_alloc_context3(codec);
		if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
		    avcodec_open2(dec, codec, nullptr) < 0) {
			avcodec_free_context(&dec);
			avformat_close_input(&fmt);
			return;
		}

		AVChannelLayout inLayout{};
		if (dec->ch_layout.nb_channels > 0)
			av_channel_layout_copy(&inLayout, &dec->ch_layout);
		else
			av_channel_layout_default(&inLayout, 2);
		AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;

		SwrContext *swr = nullptr;
		if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, 48000, &inLayout,
					dec->sample_fmt, dec->sample_rate > 0 ? dec->sample_rate : 48000, 0,
					nullptr) < 0 ||
		    swr_init(swr) < 0) {
			av_channel_layout_uninit(&inLayout);
			swr_free(&swr);
			avcodec_free_context(&dec);
			avformat_close_input(&fmt);
			return;
		}
		av_channel_layout_uninit(&inLayout);

		static const pa_sample_spec ss{PA_SAMPLE_S16LE, 48000, 2};
		int paErr = 0;
		pa_simple *pa = pa_simple_new(nullptr, "ntchffs-overlay", PA_STREAM_PLAYBACK, nullptr,
					      "Overlay Alert", &ss, nullptr, nullptr, &paErr);
		if (pa) {
			AVPacket *pkt = av_packet_alloc();
			AVFrame *frame = av_frame_alloc();
			auto flushFrame = [&](AVFrame *f) {
				int maxOut = av_rescale_rnd(swr_get_delay(swr, dec->sample_rate) + f->nb_samples,
							    48000, dec->sample_rate, AV_ROUND_UP);
				std::vector<int16_t> buf(static_cast<size_t>(std::max(1, maxOut)) * 2);
				uint8_t *outPtr[1] = {reinterpret_cast<uint8_t *>(buf.data())};
				int got = swr_convert(swr, outPtr, maxOut,
						      const_cast<const uint8_t **>(f->data), f->nb_samples);
				if (got > 0) {
					int err = 0;
					pa_simple_write(pa, buf.data(),
							static_cast<size_t>(got) * 2 * sizeof(int16_t), &err);
				}
			};
			while (av_read_frame(fmt, pkt) >= 0) {
				if (pkt->stream_index == aIdx && avcodec_send_packet(dec, pkt) >= 0) {
					while (avcodec_receive_frame(dec, frame) >= 0) {
						flushFrame(frame);
						av_frame_unref(frame);
					}
				}
				av_packet_unref(pkt);
			}
			avcodec_send_packet(dec, nullptr);
			while (avcodec_receive_frame(dec, frame) >= 0) {
				flushFrame(frame);
				av_frame_unref(frame);
			}
			int err = 0;
			pa_simple_drain(pa, &err);
			pa_simple_free(pa);
			av_packet_free(&pkt);
			av_frame_free(&frame);
		}
		swr_free(&swr);
		avcodec_free_context(&dec);
		avformat_close_input(&fmt);
	}).detach();
}

} // namespace

class OverlayIndicators::IndicatorWidget : public QWidget {
public:
	explicit IndicatorWidget(OverlayIndicators *owner) : QWidget(nullptr), owner_(owner) {}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (!owner_ || width() <= 0 || height() <= 0)
			return;
		const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
		const int pw = std::max(1, static_cast<int>(std::lround(width() * dpr)));
		const int ph = std::max(1, static_cast<int>(std::lround(height() * dpr)));
		QImage img(pw, ph, QImage::Format_ARGB32_Premultiplied);
		img.setDevicePixelRatio(dpr);
		img.fill(Qt::transparent);
		owner_->RenderToImage(img);

		QPainter p(this);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(QPointF(0, 0), img);
	}

private:
	OverlayIndicators *owner_ = nullptr;
};

OverlayIndicators::OverlayIndicators(QObject *parent) : QObject(parent)
{
	animTimer_.setInterval(33);
	connect(&animTimer_, &QTimer::timeout, this, [this]() {
		if (!widget_)
			return;
		uint64_t now = overlay_compat::GetTickCount64Ms();
		if (savedUntilTick_ > 0 && now >= savedUntilTick_) {
			savedUntilTick_ = 0;
			UpdateVisibilityAndTimer();
			return;
		}
		if (widget_->isVisible())
			widget_->update();
	});
}

OverlayIndicators::~OverlayIndicators()
{
	Shutdown();
}

bool OverlayIndicators::Initialize()
{
	if (widget_)
		return true;
	widget_ = new IndicatorWidget(this);
	RebuildGeometry();
	return true;
}

void OverlayIndicators::Shutdown()
{
	animTimer_.stop();
	if (widget_) {
		widget_->hide();
		widget_->deleteLater();
		widget_ = nullptr;
	}
}

uint32_t OverlayIndicators::DefaultSlotColor(int slot) const
{
	switch (slot) {
	case SlotRecording:
		return 0xEF4444;
	case SlotReplay:
		return 0x22C55E;
	case SlotSaving:
		return 0xF59E0B;
	case SlotSaved:
		return 0x38BDF8;
	default:
		return 0xFFFFFF;
	}
}

void OverlayIndicators::SetEnabled(bool enabled)
{
	enabled_ = enabled;
	UpdateVisibilityAndTimer();
}

void OverlayIndicators::SetPosition(OverlayPosition pos)
{
	position_ = pos;
	RebuildGeometry();
}

void OverlayIndicators::SetOledProtection(bool enabled)
{
	oledProtection_ = enabled;
	if (widget_ && widget_->isVisible())
		widget_->update();
}

void OverlayIndicators::SetScale(int scalePercent)
{
	scalePercent_ = std::clamp(scalePercent, 50, 250);
	RebuildGeometry();
}

void OverlayIndicators::SetPreview(bool preview)
{
	previewMode_ = preview;
	UpdateVisibilityAndTimer();
}

void OverlayIndicators::SetMonitor(OverlayMonitorMode mode, int monitorIndex)
{
	monitorMode_ = mode;
	monitorIndex_ = monitorIndex;
	RebuildGeometry();
}

void OverlayIndicators::SetCustomIcon(int slot, const std::string &path, bool tint, uint32_t colorRgb)
{
	if (slot < 0 || slot >= CUSTOM_SLOT_COUNT)
		return;
	auto &cfg = slots_[slot];
	cfg.path = path;
	cfg.tint = tint;
	cfg.colorRgb = colorRgb;
	cfg.frames.Clear();
	if (!path.empty()) {
		int px = std::max(32, static_cast<int>(std::lround(40.0 * scalePercent_ / 100.0)));
		if (!cfg.frames.LoadFromFile(path, tint, colorRgb, px)) {
			obs_log(LOG_WARNING, "Indicators: failed to load custom icon for slot %d", slot);
		}
	}
	if (widget_ && widget_->isVisible())
		widget_->update();
}

void OverlayIndicators::SetSoundPath(int eventIdx, const std::string &path)
{
	if (eventIdx < 0 || eventIdx >= SOUND_EVENT_COUNT)
		return;
	soundPaths_[eventIdx] = path;
}

bool OverlayIndicators::PlaySoundEvent(SoundEvent ev)
{
	int idx = static_cast<int>(ev);
	if (idx < 0 || idx >= SOUND_EVENT_COUNT || soundPaths_[idx].empty())
		return false;
	PlayAudioFileAsync(soundPaths_[idx]);
	return true;
}

void OverlayIndicators::UpdateState(bool recordingActive, bool replayActive, bool replaySaving)
{
	uint64_t now = overlay_compat::GetTickCount64Ms();
	if (recordingActive != recordingActive_) {
		recordingActive_ = recordingActive;
		recStartTick_ = recordingActive ? now : 0;
	}
	if (replayActive != replayActive_) {
		replayActive_ = replayActive;
		replayStartTick_ = replayActive ? now : 0;
	}
	if (replaySaving != replaySaving_) {
		replaySaving_ = replaySaving;
		savingStartTick_ = replaySaving ? now : 0;
	}
	UpdateVisibilityAndTimer();
}

void OverlayIndicators::NotifyReplaySaved()
{
	uint64_t now = overlay_compat::GetTickCount64Ms();
	savedStartTick_ = now;
	savedUntilTick_ = now + 2600;
	UpdateVisibilityAndTimer();
}

float OverlayIndicators::SlotOpacity(uint64_t nowMs, uint64_t startTickMs) const
{
	if (!oledProtection_)
		return 1.0f;
	if (startTickMs == 0 || nowMs < startTickMs)
		return 0.35f;
	uint64_t elapsed = nowMs - startTickMs;
	if (elapsed < 5000)
		return 1.0f;
	if (elapsed >= 7000)
		return 0.35f;
	float t = static_cast<float>(elapsed - 5000) / 2000.0f;
	return 1.0f - 0.65f * t;
}

void OverlayIndicators::RebuildGeometry()
{
	if (!widget_)
		return;

	const float s = static_cast<float>(scalePercent_) / 100.0f;
	const int itemSz = static_cast<int>(std::lround(34.0f * s));
	const int gap = static_cast<int>(std::lround(8.0f * s));
	const int pad = static_cast<int>(std::lround(8.0f * s));

	int count = 0;
	if (previewMode_) {
		count = 4;
	} else {
		if (recordingActive_)
			++count;
		if (replayActive_)
			++count;
		if (replaySaving_)
			++count;
		if (savedUntilTick_ > overlay_compat::GetTickCount64Ms())
			++count;
	}
	count = std::max(1, count);

	const int w = pad * 2 + count * itemSz + (count - 1) * gap;
	const int h = pad * 2 + itemSz;
	const int margin = std::max(8, (scalePercent_ * 20) / 100);

	QScreen *targetScreen = overlay_win::OverlayWindowManager::ResolveTargetScreen(monitorMode_, monitorIndex_);

	overlay_win::LayerSurfaceSpec spec;
	spec.layer = overlay_win::LayerLevel::Overlay;
	spec.exclusiveZone = -1;
	spec.keyboardMode = overlay_win::KeyboardMode::None;
	spec.scope = QStringLiteral("ntchffs-overlay-indicators");
	spec.targetScreen = targetScreen;
	spec.clickThrough = true;
	spec.desiredSize = QSize(w, h);
	spec.margins = QMargins(margin, margin, margin, margin);

	switch (position_) {
	case OverlayPosition::Top:
		spec.anchor = overlay_win::AnchorTop;
		break;
	case OverlayPosition::Bottom:
		spec.anchor = overlay_win::AnchorBottom;
		break;
	case OverlayPosition::Left:
		spec.anchor = overlay_win::AnchorLeft;
		break;
	case OverlayPosition::Right:
		spec.anchor = overlay_win::AnchorRight;
		break;
	case OverlayPosition::TopLeft:
		spec.anchor = overlay_win::AnchorTop | overlay_win::AnchorLeft;
		break;
	case OverlayPosition::TopRight:
		spec.anchor = overlay_win::AnchorTop | overlay_win::AnchorRight;
		break;
	case OverlayPosition::BottomLeft:
		spec.anchor = overlay_win::AnchorBottom | overlay_win::AnchorLeft;
		break;
	case OverlayPosition::BottomRight:
		spec.anchor = overlay_win::AnchorBottom | overlay_win::AnchorRight;
		break;
	case OverlayPosition::Center:
		spec.anchor = overlay_win::AnchorNone;
		break;
	}

	widget_->setFixedSize(w, h);
	if (overlay_win::WaylandLayerHelper::IsX11Platform() && targetScreen) {
		QRect sr = targetScreen->geometry();
		int x = sr.x() + margin;
		int y = sr.y() + margin;
		switch (position_) {
		case OverlayPosition::Top:
			x = sr.x() + (sr.width() - w) / 2;
			y = sr.y() + margin;
			break;
		case OverlayPosition::Bottom:
			x = sr.x() + (sr.width() - w) / 2;
			y = sr.y() + sr.height() - h - margin;
			break;
		case OverlayPosition::Left:
			x = sr.x() + margin;
			y = sr.y() + (sr.height() - h) / 2;
			break;
		case OverlayPosition::Right:
			x = sr.x() + sr.width() - w - margin;
			y = sr.y() + (sr.height() - h) / 2;
			break;
		case OverlayPosition::TopLeft:
			x = sr.x() + margin;
			y = sr.y() + margin;
			break;
		case OverlayPosition::TopRight:
			x = sr.x() + sr.width() - w - margin;
			y = sr.y() + margin;
			break;
		case OverlayPosition::BottomLeft:
			x = sr.x() + margin;
			y = sr.y() + sr.height() - h - margin;
			break;
		case OverlayPosition::BottomRight:
			x = sr.x() + sr.width() - w - margin;
			y = sr.y() + sr.height() - h - margin;
			break;
		case OverlayPosition::Center:
			x = sr.x() + (sr.width() - w) / 2;
			y = sr.y() + (sr.height() - h) / 2;
			break;
		}
		widget_->move(x, y);
	}

	if (widget_->windowHandle() && widget_->windowHandle()->handle())
		overlay_win::WaylandLayerHelper::UpdateWidgetSpec(widget_, spec);
	else
		overlay_win::WaylandLayerHelper::ConfigureWidget(widget_, spec);
}

void OverlayIndicators::UpdateVisibilityAndTimer()
{
	if (!widget_)
		return;
	uint64_t now = overlay_compat::GetTickCount64Ms();
	bool anyActive = previewMode_ || recordingActive_ || replayActive_ || replaySaving_ ||
			 (savedUntilTick_ > now);
	bool shouldShow = enabled_ && anyActive;

	if (shouldShow) {
		RebuildGeometry();
		if (!widget_->isVisible())
			widget_->show();
		widget_->update();
		if (!animTimer_.isActive())
			animTimer_.start();
	} else {
		if (widget_->isVisible())
			widget_->hide();
		animTimer_.stop();
	}
}

void OverlayIndicators::RenderToImage(QImage &canvas)
{
	uint64_t now = overlay_compat::GetTickCount64Ms();

	struct ActiveItem {
		int slot;
		uint64_t startTick;
	};
	std::vector<ActiveItem> items;
	if (previewMode_) {
		items.push_back({SlotRecording, now});
		items.push_back({SlotReplay, now});
		items.push_back({SlotSaving, now});
		items.push_back({SlotSaved, now});
	} else {
		if (recordingActive_)
			items.push_back({SlotRecording, recStartTick_});
		if (replayActive_)
			items.push_back({SlotReplay, replayStartTick_});
		if (replaySaving_)
			items.push_back({SlotSaving, savingStartTick_});
		if (savedUntilTick_ > now)
			items.push_back({SlotSaved, savedStartTick_});
	}
	if (items.empty())
		return;

	const float s = static_cast<float>(scalePercent_) / 100.0f;
	const float itemSz = 34.0f * s;
	const float gap = 8.0f * s;
	const float pad = 8.0f * s;

	QPainter p(&canvas);
	overlay_compat::Canvas c(&p);

	const float w = static_cast<float>(widget_->width());
	const float h = static_cast<float>(widget_->height());

	// Translucent dark pill background modulated gently when OLED fade active
	float minOp = 1.0f;
	for (const auto &it : items)
		minOp = std::max(minOp, SlotOpacity(now, it.startTick));
	c.FillRoundedRect(overlay_compat::MakeRoundedRect(overlay_compat::MakeRectXYWH(0, 0, w, h),
							  10.0f * s, 10.0f * s),
			  overlay_compat::ColorF(0x111319, 0.72f * minOp));
	c.DrawRoundedRect(overlay_compat::MakeRoundedRect(overlay_compat::MakeRectXYWH(0.5f, 0.5f, w - 1.0f, h - 1.0f),
							  10.0f * s, 10.0f * s),
			  overlay_compat::ColorF(0xFFFFFF, 0.14f * minOp), 1.0f);

	for (size_t i = 0; i < items.size(); ++i) {
		int slot = items[i].slot;
		float op = SlotOpacity(now, items[i].startTick);
		float x = pad + static_cast<float>(i) * (itemSz + gap);
		float y = pad;
		auto rect = overlay_compat::MakeRectXYWH(x, y, itemSz, itemSz);

		auto &cfg = slots_[slot];
		if (cfg.frames.IsLoaded()) {
			if (const auto *bmp = cfg.frames.CurrentBitmap(now)) {
				c.DrawBitmap(bmp, rect, op);
				continue;
			}
		}

		uint32_t colHex = cfg.tint ? cfg.colorRgb : DefaultSlotColor(slot);
		overlay_compat::ColorF col(colHex, op);
		float cx = x + itemSz * 0.5f;
		float cy = y + itemSz * 0.5f;
		float r = itemSz * 0.34f;

		if (slot == SlotRecording) {
			float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(now % 1400) / 1400.0f * 6.28318f);
			c.FillEllipse({{cx, cy}, r * 1.25f, r * 1.25f}, overlay_compat::ColorF(colHex, op * 0.25f * pulse));
			c.FillEllipse({{cx, cy}, r * 0.75f, r * 0.75f}, col);
		} else if (slot == SlotReplay) {
			c.DrawEllipse({{cx, cy}, r, r}, col, 2.5f * s);
			c.FillEllipse({{cx, cy}, r * 0.32f, r * 0.32f}, col);
		} else if (slot == SlotSaving) {
			float ang = static_cast<float>(now % 900) / 900.0f * 360.0f;
			p.save();
			p.setPen(QPen(col.ToQColor(), 2.5f * s, Qt::SolidLine, Qt::RoundCap));
			p.drawArc(QRectF(cx - r, cy - r, r * 2.0f, r * 2.0f),
				  static_cast<int>(-ang * 16), 240 * 16);
			p.restore();
		} else {
			QPainterPath check;
			check.moveTo(cx - r * 0.6f, cy + r * 0.05f);
			check.lineTo(cx - r * 0.1f, cy + r * 0.55f);
			check.lineTo(cx + r * 0.65f, cy - r * 0.45f);
			c.StrokePath(check, col, 2.8f * s);
		}
	}
}
