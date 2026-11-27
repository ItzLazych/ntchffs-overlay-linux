#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QTransform>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "overlay-string-utils.h"

namespace overlay_compat {

inline uint64_t GetTickCount64Ms()
{
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct Point2F {
	float x = 0.0f;
	float y = 0.0f;
};

struct SizeF {
	float width = 0.0f;
	float height = 0.0f;
};

struct SizeU {
	uint32_t width = 0;
	uint32_t height = 0;
};

struct RectF {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;

	float Width() const { return right - left; }
	float Height() const { return bottom - top; }
	QRectF ToQRectF() const { return QRectF(left, top, right - left, bottom - top); }
	bool Contains(float x, float y) const
	{
		return x >= left && x <= right && y >= top && y <= bottom;
	}
};

struct RoundedRect {
	RectF rect;
	float radiusX = 0.0f;
	float radiusY = 0.0f;
};

struct Ellipse {
	Point2F point;
	float radiusX = 0.0f;
	float radiusY = 0.0f;
};

struct ColorF {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 1.0f;

	ColorF() = default;
	ColorF(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
	explicit ColorF(uint32_t rgb, float a_ = 1.0f)
		: r(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f),
		  g(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f),
		  b(static_cast<float>(rgb & 0xFF) / 255.0f),
		  a(a_)
	{
	}

	QColor ToQColor() const
	{
		auto clamp255 = [](float v) {
			return static_cast<int>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
		};
		return QColor(clamp255(r), clamp255(g), clamp255(b), clamp255(a));
	}
};

inline ColorF ColorFromWinColorRef(uint32_t colorRef, float alpha = 1.0f)
{
	// Windows COLORREF is 0x00BBGGRR.
	uint8_t r = static_cast<uint8_t>(colorRef & 0xFF);
	uint8_t g = static_cast<uint8_t>((colorRef >> 8) & 0xFF);
	uint8_t b = static_cast<uint8_t>((colorRef >> 16) & 0xFF);
	return ColorF(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
}

inline uint32_t ColorToWinColorRef(uint8_t r, uint8_t g, uint8_t b)
{
	return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
	       (static_cast<uint32_t>(b) << 16);
}

inline RectF MakeRectF(float l, float t, float r, float b)
{
	return RectF{l, t, r, b};
}

inline RectF MakeRectXYWH(float x, float y, float w, float h)
{
	return RectF{x, y, x + w, y + h};
}

inline RoundedRect MakeRoundedRect(const RectF &r, float rx, float ry)
{
	return RoundedRect{r, rx, ry};
}

inline Point2F MakePoint2F(float x, float y)
{
	return Point2F{x, y};
}

enum class TextAlign { Leading, Trailing, Center };
enum class ParagraphAlign { Near, Far, Center };
enum class WordWrap { Wrap, NoWrap };

class TextFormat {
public:
	TextFormat() = default;
	TextFormat(const QString &family, float sizePx, QFont::Weight weight = QFont::Normal,
		   bool italic = false)
		: font_(family), sizePx_(sizePx)
	{
		font_.setPixelSize(std::max(1, static_cast<int>(std::lround(sizePx))));
		font_.setWeight(weight);
		font_.setItalic(italic);
		font_.setStyleStrategy(QFont::PreferAntialias);
	}

	void SetTextAlignment(TextAlign align) { hAlign_ = align; }
	void SetParagraphAlignment(ParagraphAlign align) { vAlign_ = align; }
	void SetWordWrapping(WordWrap wrap) { wrap_ = wrap; }
	void SetTrimmingEllipsis(bool enable) { ellipsis_ = enable; }
	void SetEllipsis(bool enable) { ellipsis_ = enable; }

	const QFont &Font() const { return font_; }
	float SizePx() const { return sizePx_; }
	TextAlign HAlign() const { return hAlign_; }
	ParagraphAlign VAlign() const { return vAlign_; }
	WordWrap Wrapping() const { return wrap_; }
	bool Ellipsis() const { return ellipsis_; }

	int QtFlags() const
	{
		int flags = 0;
		switch (hAlign_) {
		case TextAlign::Leading:
			flags |= Qt::AlignLeft;
			break;
		case TextAlign::Trailing:
			flags |= Qt::AlignRight;
			break;
		case TextAlign::Center:
			flags |= Qt::AlignHCenter;
			break;
		}
		switch (vAlign_) {
		case ParagraphAlign::Near:
			flags |= Qt::AlignTop;
			break;
		case ParagraphAlign::Far:
			flags |= Qt::AlignBottom;
			break;
		case ParagraphAlign::Center:
			flags |= Qt::AlignVCenter;
			break;
		}
		if (wrap_ == WordWrap::Wrap)
			flags |= Qt::TextWordWrap;
		else
			flags |= Qt::TextSingleLine;
		return flags;
	}

	SizeF Measure(const QString &text, float maxWidth = 10000.0f) const
	{
		QFontMetricsF fm(font_);
		if (wrap_ == WordWrap::NoWrap) {
			QRectF br = fm.boundingRect(QRectF(0, 0, maxWidth, 10000.0), Qt::AlignLeft | Qt::TextSingleLine,
						    text);
			return {static_cast<float>(fm.horizontalAdvance(text)),
				static_cast<float>(std::max(br.height(), fm.height()))};
		}
		QRectF br = fm.boundingRect(QRectF(0, 0, maxWidth, 10000.0), Qt::AlignLeft | Qt::TextWordWrap, text);
		return {static_cast<float>(br.width()), static_cast<float>(br.height())};
	}

private:
	QFont font_;
	float sizePx_ = 14.0f;
	TextAlign hAlign_ = TextAlign::Leading;
	ParagraphAlign vAlign_ = ParagraphAlign::Near;
	WordWrap wrap_ = WordWrap::NoWrap;
	bool ellipsis_ = false;
};

class Bitmap {
public:
	Bitmap() = default;
	explicit Bitmap(QImage img)
		: image_(img.format() == QImage::Format_ARGB32_Premultiplied
				 ? std::move(img)
				 : img.convertToFormat(QImage::Format_ARGB32_Premultiplied))
	{
	}

	static std::shared_ptr<Bitmap> CreateFromBgraPremul(int width, int height, const uint8_t *data, int stride)
	{
		if (width <= 0 || height <= 0 || !data)
			return nullptr;
		QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
		for (int y = 0; y < height; ++y) {
			std::memcpy(img.scanLine(y), data + y * stride, static_cast<size_t>(width) * 4);
		}
		return std::make_shared<Bitmap>(std::move(img));
	}

	void UpdateFromBgraPremul(int width, int height, const uint8_t *data, int stride)
	{
		if (width <= 0 || height <= 0 || !data)
			return;
		if (image_.width() != width || image_.height() != height ||
		    image_.format() != QImage::Format_ARGB32_Premultiplied) {
			image_ = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
		}
		for (int y = 0; y < height; ++y) {
			std::memcpy(image_.scanLine(y), data + y * stride, static_cast<size_t>(width) * 4);
		}
	}

	bool IsValid() const { return !image_.isNull(); }
	int Width() const { return image_.width(); }
	int Height() const { return image_.height(); }
	const QImage &Image() const { return image_; }
	QImage &Image() { return image_; }

private:
	QImage image_;
};

class Canvas {
public:
	Canvas(QPainter *painter, float dpiScale = 1.0f) : painter_(painter), dpiScale_(dpiScale)
	{
		if (painter_) {
			painter_->setRenderHint(QPainter::Antialiasing, true);
			painter_->setRenderHint(QPainter::TextAntialiasing, true);
			painter_->setRenderHint(QPainter::SmoothPixmapTransform, true);
			if (std::abs(dpiScale_ - 1.0f) > 0.001f) {
				baseTransform_ = QTransform::fromScale(dpiScale_, dpiScale_);
				painter_->setTransform(baseTransform_);
			}
		}
	}

	QPainter *Painter() const { return painter_; }

	void Clear(const ColorF &c)
	{
		if (!painter_)
			return;
		painter_->save();
		painter_->setCompositionMode(QPainter::CompositionMode_Source);
		painter_->fillRect(painter_->viewport(), c.ToQColor());
		painter_->restore();
	}

	void SetTransform(const QTransform &t)
	{
		if (!painter_)
			return;
		painter_->setTransform(t * baseTransform_);
	}

	void ResetTransform()
	{
		if (!painter_)
			return;
		painter_->setTransform(baseTransform_);
	}

	void PushClip(const RectF &rect)
	{
		if (!painter_)
			return;
		painter_->save();
		painter_->setClipRect(rect.ToQRectF(), Qt::IntersectClip);
	}

	void PushRoundedClip(const RoundedRect &rr)
	{
		if (!painter_)
			return;
		painter_->save();
		QPainterPath path;
		path.addRoundedRect(rr.rect.ToQRectF(), rr.radiusX, rr.radiusY);
		painter_->setClipPath(path, Qt::IntersectClip);
	}

	void PopClip()
	{
		if (!painter_)
			return;
		painter_->restore();
	}

	void FillRect(const RectF &r, const ColorF &c)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->fillRect(r.ToQRectF(), c.ToQColor());
	}

	void FillRectGradient(const RectF &r, const QGradient &grad)
	{
		if (!painter_)
			return;
		painter_->fillRect(r.ToQRectF(), QBrush(grad));
	}

	void DrawRect(const RectF &r, const ColorF &c, float stroke = 1.0f)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(QPen(c.ToQColor(), stroke));
		painter_->setBrush(Qt::NoBrush);
		painter_->drawRect(r.ToQRectF());
	}

	void FillRoundedRect(const RoundedRect &rr, const ColorF &c)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(Qt::NoPen);
		painter_->setBrush(c.ToQColor());
		painter_->drawRoundedRect(rr.rect.ToQRectF(), rr.radiusX, rr.radiusY);
	}

	void FillRoundedRectBrush(const RoundedRect &rr, const QBrush &brush)
	{
		if (!painter_)
			return;
		painter_->setPen(Qt::NoPen);
		painter_->setBrush(brush);
		painter_->drawRoundedRect(rr.rect.ToQRectF(), rr.radiusX, rr.radiusY);
	}

	void DrawRoundedRect(const RoundedRect &rr, const ColorF &c, float stroke = 1.0f)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(QPen(c.ToQColor(), stroke));
		painter_->setBrush(Qt::NoBrush);
		painter_->drawRoundedRect(rr.rect.ToQRectF(), rr.radiusX, rr.radiusY);
	}

	void FillEllipse(const Ellipse &e, const ColorF &c)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(Qt::NoPen);
		painter_->setBrush(c.ToQColor());
		painter_->drawEllipse(QPointF(e.point.x, e.point.y), e.radiusX, e.radiusY);
	}

	void DrawEllipse(const Ellipse &e, const ColorF &c, float stroke = 1.0f)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(QPen(c.ToQColor(), stroke));
		painter_->setBrush(Qt::NoBrush);
		painter_->drawEllipse(QPointF(e.point.x, e.point.y), e.radiusX, e.radiusY);
	}

	void DrawLine(const Point2F &p0, const Point2F &p1, const ColorF &c, float stroke = 1.0f,
		      Qt::PenCapStyle cap = Qt::RoundCap)
	{
		if (!painter_ || c.a <= 0.0f)
			return;
		painter_->setPen(QPen(c.ToQColor(), stroke, Qt::SolidLine, cap, Qt::RoundJoin));
		painter_->drawLine(QPointF(p0.x, p0.y), QPointF(p1.x, p1.y));
	}

	void DrawBitmap(const Bitmap *bmp, const RectF &dst, float opacity = 1.0f,
			const RectF *src = nullptr)
	{
		if (!painter_ || !bmp || !bmp->IsValid() || opacity <= 0.0f)
			return;
		painter_->save();
		if (opacity < 0.999f)
			painter_->setOpacity(opacity);
		QRectF srcRect = src ? src->ToQRectF() : QRectF(0, 0, bmp->Width(), bmp->Height());
		painter_->drawImage(dst.ToQRectF(), bmp->Image(), srcRect);
		painter_->restore();
	}

	void DrawImage(const QImage &img, const RectF &dst, float opacity = 1.0f, const RectF *src = nullptr)
	{
		if (!painter_ || img.isNull() || opacity <= 0.0f)
			return;
		painter_->save();
		if (opacity < 0.999f)
			painter_->setOpacity(opacity);
		QRectF srcRect = src ? src->ToQRectF() : QRectF(0, 0, img.width(), img.height());
		painter_->drawImage(dst.ToQRectF(), img, srcRect);
		painter_->restore();
	}

	void DrawText(const QString &text, const TextFormat &fmt, const RectF &rect, const ColorF &color)
	{
		if (!painter_ || color.a <= 0.0f || text.isEmpty())
			return;
		painter_->setFont(fmt.Font());
		painter_->setPen(color.ToQColor());
		QString drawn = text;
		if (fmt.Ellipsis() && fmt.Wrapping() == WordWrap::NoWrap) {
			QFontMetricsF fm(fmt.Font());
			drawn = fm.elidedText(text, Qt::ElideRight, std::max(0.0, static_cast<double>(rect.Width())));
		}
		painter_->drawText(rect.ToQRectF(), fmt.QtFlags(), drawn);
	}

	void DrawTextUtf8(const std::string &utf8, const TextFormat &fmt, const RectF &rect, const ColorF &color)
	{
		DrawText(overlay_str::Utf8ToQString(utf8), fmt, rect, color);
	}

	void FillPath(const QPainterPath &path, const ColorF &color)
	{
		if (!painter_ || color.a <= 0.0f)
			return;
		painter_->setPen(Qt::NoPen);
		painter_->setBrush(color.ToQColor());
		painter_->drawPath(path);
	}

	void StrokePath(const QPainterPath &path, const ColorF &color, float width = 1.5f,
			Qt::PenCapStyle cap = Qt::RoundCap, Qt::PenJoinStyle join = Qt::RoundJoin)
	{
		if (!painter_ || color.a <= 0.0f)
			return;
		painter_->setPen(QPen(color.ToQColor(), width, Qt::SolidLine, cap, join));
		painter_->setBrush(Qt::NoBrush);
		painter_->drawPath(path);
	}

private:
	QPainter *painter_ = nullptr;
	float dpiScale_ = 1.0f;
	QTransform baseTransform_;
};

} // namespace overlay_compat
