#pragma once

#include <QImage>
#include <cstdint>
#include <string>

namespace overlay::svg {

bool RasterizeSvgFile(const std::string &path, int targetWidth, int targetHeight, QImage &outImage);
bool RasterizeSvgData(const QByteArray &svgBytes, int targetWidth, int targetHeight, QImage &outImage);

} // namespace overlay::svg
