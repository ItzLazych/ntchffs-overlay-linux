#pragma once

#include <QString>
#include <cstdint>
#include <string>

namespace overlay_str {

std::wstring Utf8ToWide(const std::string &utf8);
std::string WideToUtf8(const std::wstring &wide);

QString Utf8ToQString(const std::string &utf8);
std::string QStringToUtf8(const QString &str);

std::string ToLowerAscii(const std::string &s);
bool EndsWithCaseInsensitive(const std::string &str, const std::string &suffix);
std::string TrimAscii(const std::string &s);

std::string FileNameFromPath(const std::string &path);
std::string ParentDirFromPath(const std::string &path);
std::string ExtensionLowerFromPath(const std::string &path);

std::string FormatFileSize(uint64_t bytes);
std::string FormatDurationMmSs(double seconds);

} // namespace overlay_str
