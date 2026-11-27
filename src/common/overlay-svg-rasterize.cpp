#include "overlay-svg-rasterize.h"

#include <QFile>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

#include "overlay-string-utils.h"

namespace overlay::svg {

static bool RenderSvgWithRenderer(QSvgRenderer &renderer, int targetWidth, int targetHeight, QImage &outImage)
{
	if (!renderer.isValid())
		return false;

	QSize defSize = renderer.defaultSize();
	int w = targetWidth > 0 ? targetWidth : (defSize.width() > 0 ? defSize.width() : 64);
	int h = targetHeight > 0 ? targetHeight : (defSize.height() > 0 ? defSize.height() : 64);
	if (w <= 0 || h <= 0)
		return false;

	QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);

	QPainter painter(&img);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

	if (defSize.width() > 0 && defSize.height() > 0) {
		QSizeF scaled = QSizeF(defSize).scaled(QSizeF(w, h), Qt::KeepAspectRatio);
		double ox = (w - scaled.width()) * 0.5;
		double oy = (h - scaled.height()) * 0.5;
		renderer.render(&painter, QRectF(ox, oy, scaled.width(), scaled.height()));
	} else {
		renderer.render(&painter, QRectF(0, 0, w, h));
	}
	painter.end();

	outImage = std::move(img);
	return true;
}

bool RasterizeSvgFile(const std::string &path, int targetWidth, int targetHeight, QImage &outImage)
{
	if (path.empty())
		return false;
	QSvgRenderer renderer(overlay_str::Utf8ToQString(path));
	return RenderSvgWithRenderer(renderer, targetWidth, targetHeight, outImage);
}

bool RasterizeSvgData(const QByteArray &svgBytes, int targetWidth, int targetHeight, QImage &outImage)
{
	if (svgBytes.isEmpty())
		return false;
	QSvgRenderer renderer(svgBytes);
	return RenderSvgWithRenderer(renderer, targetWidth, targetHeight, outImage);
}

} // namespace overlay::svg
