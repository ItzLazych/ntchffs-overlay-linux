#include "overlay-string-utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace overlay_str {

std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	return QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size())).toStdWString();
}

std::string WideToUtf8(const std::wstring &wide)
{
	if (wide.empty())
		return {};
	QByteArray bytes = QString::fromStdWString(wide).toUtf8();
	return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

QString Utf8ToQString(const std::string &utf8)
{
	return QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size()));
}

std::string QStringToUtf8(const QString &str)
{
	QByteArray bytes = str.toUtf8();
	return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

std::string ToLowerAscii(const std::string &s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

bool EndsWithCaseInsensitive(const std::string &str, const std::string &suffix)
{
	if (suffix.size() > str.size())
		return false;
	return ToLowerAscii(str.substr(str.size() - suffix.size())) == ToLowerAscii(suffix);
}

std::string TrimAscii(const std::string &s)
{
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
		++start;
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
		--end;
	return s.substr(start, end - start);
}

std::string FileNameFromPath(const std::string &path)
{
	if (path.empty())
		return {};
	std::error_code ec;
	std::filesystem::path p(path);
	auto fn = p.filename().string();
	return fn;
}

std::string ParentDirFromPath(const std::string &path)
{
	if (path.empty())
		return {};
	std::filesystem::path p(path);
	return p.parent_path().string();
}

std::string ExtensionLowerFromPath(const std::string &path)
{
	if (path.empty())
		return {};
	std::filesystem::path p(path);
	return ToLowerAscii(p.extension().string());
}

std::string FormatFileSize(uint64_t bytes)
{
	char buf[64];
	const double d = static_cast<double>(bytes);
	if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
		std::snprintf(buf, sizeof(buf), "%.2f GB", d / (1024.0 * 1024.0 * 1024.0));
	} else if (bytes >= 1024ULL * 1024ULL) {
		std::snprintf(buf, sizeof(buf), "%.1f MB", d / (1024.0 * 1024.0));
	} else if (bytes >= 1024ULL) {
		std::snprintf(buf, sizeof(buf), "%.1f KB", d / 1024.0);
	} else {
		std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	}
	return buf;
}

std::string FormatDurationMmSs(double seconds)
{
	if (!std::isfinite(seconds) || seconds < 0.0)
		seconds = 0.0;
	int total = static_cast<int>(std::floor(seconds + 0.0005));
	int hours = total / 3600;
	int mins = (total % 3600) / 60;
	int secs = total % 60;
	char buf[32];
	if (hours > 0)
		std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, mins, secs);
	else
		std::snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
	return buf;
}

} // namespace overlay_str
