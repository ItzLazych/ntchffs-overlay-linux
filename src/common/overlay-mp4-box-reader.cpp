#include "overlay-mp4-box-reader.h"

#include <QString>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace overlay_mp4 {

namespace {

uint32_t ReadBe32(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t ReadBe64(const uint8_t *p)
{
	return (static_cast<uint64_t>(ReadBe32(p)) << 32) | static_cast<uint64_t>(ReadBe32(p + 4));
}

constexpr uint32_t FourCC(const char (&s)[5])
{
	return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) |
	       (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
	       (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
	       static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

struct BoxHeader {
	uint64_t offset = 0;
	uint64_t size = 0;
	uint64_t headerSize = 0;
	uint32_t type = 0;
};

bool ReadBoxHeader(std::ifstream &fs, uint64_t offset, uint64_t limitEnd, BoxHeader &out)
{
	if (offset + 8 > limitEnd)
		return false;
	fs.seekg(static_cast<std::streamoff>(offset));
	if (!fs)
		return false;

	uint8_t buf[16]{};
	fs.read(reinterpret_cast<char *>(buf), 8);
	if (fs.gcount() != 8)
		return false;

	uint64_t boxSize = ReadBe32(buf);
	uint32_t boxType = ReadBe32(buf + 4);
	uint64_t hdrSize = 8;

	if (boxSize == 1) {
		if (offset + 16 > limitEnd)
			return false;
		fs.read(reinterpret_cast<char *>(buf + 8), 8);
		if (fs.gcount() != 8)
			return false;
		boxSize = ReadBe64(buf + 8);
		hdrSize = 16;
	} else if (boxSize == 0) {
		boxSize = limitEnd - offset;
	}

	if (boxSize < hdrSize || offset + boxSize > limitEnd)
		return false;

	out.offset = offset;
	out.size = boxSize;
	out.headerSize = hdrSize;
	out.type = boxType;
	return true;
}

std::string CleanAtomString(const uint8_t *data, size_t len)
{
	if (!data || len == 0)
		return {};
	// Handle UTF-16 BOM if present
	if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
		QString qs = QString::fromUtf16(reinterpret_cast<const char16_t *>(data + 2),
						static_cast<int>((len - 2) / 2));
		// byte-swap BE UTF-16
		std::u16string u16;
		u16.reserve((len - 2) / 2);
		for (size_t i = 2; i + 1 < len; i += 2) {
			char16_t ch = static_cast<char16_t>((data[i] << 8) | data[i + 1]);
			if (ch == 0)
				break;
			u16.push_back(ch);
		}
		return QString::fromStdU16String(u16).toUtf8().toStdString();
	}
	size_t actualLen = 0;
	while (actualLen < len && data[actualLen] != '\0')
		++actualLen;
	return std::string(reinterpret_cast<const char *>(data), actualLen);
}

void ParseTrak(std::ifstream &fs, uint64_t trakStart, uint64_t trakEnd, std::vector<std::string> &outNames)
{
	bool isAudio = false;
	std::string trackName;

	uint64_t pos = trakStart;
	while (pos + 8 <= trakEnd) {
		BoxHeader box;
		if (!ReadBoxHeader(fs, pos, trakEnd, box))
			break;
		uint64_t bodyStart = box.offset + box.headerSize;
		uint64_t bodyEnd = box.offset + box.size;

		if (box.type == FourCC("mdia")) {
			uint64_t mp = bodyStart;
			while (mp + 8 <= bodyEnd) {
				BoxHeader mbox;
				if (!ReadBoxHeader(fs, mp, bodyEnd, mbox))
					break;
				if (mbox.type == FourCC("hdlr")) {
					uint64_t hStart = mbox.offset + mbox.headerSize;
					// FullBox(4) + pre_defined(4) + handler_type(4)
					if (mbox.offset + mbox.size >= hStart + 12) {
						fs.seekg(static_cast<std::streamoff>(hStart + 8));
						uint8_t htype[4]{};
						fs.read(reinterpret_cast<char *>(htype), 4);
						if (fs.gcount() == 4 && ReadBe32(htype) == FourCC("soun"))
							isAudio = true;
					}
				}
				mp += mbox.size;
			}
		} else if (box.type == FourCC("udta")) {
			uint64_t up = bodyStart;
			while (up + 8 <= bodyEnd) {
				BoxHeader ubox;
				if (!ReadBoxHeader(fs, up, bodyEnd, ubox))
					break;
				if (ubox.type == FourCC("name")) {
					uint64_t payloadLen = ubox.size - ubox.headerSize;
					if (payloadLen > 0 && payloadLen <= 4096) {
						std::vector<uint8_t> buf(static_cast<size_t>(payloadLen));
						fs.seekg(static_cast<std::streamoff>(ubox.offset + ubox.headerSize));
						fs.read(reinterpret_cast<char *>(buf.data()),
							static_cast<std::streamsize>(payloadLen));
						if (fs)
							trackName = CleanAtomString(buf.data(), buf.size());
					}
				}
				up += ubox.size;
			}
		}
		pos += box.size;
	}

	if (isAudio)
		outNames.push_back(std::move(trackName));
}

} // namespace

std::vector<std::string> ReadMp4AudioTrackNames(const std::string &filePath)
{
	std::vector<std::string> names;
	if (filePath.empty())
		return names;

	std::ifstream fs(filePath, std::ios::binary | std::ios::ate);
	if (!fs)
		return names;

	std::streampos endPos = fs.tellg();
	if (endPos <= 0)
		return names;
	const uint64_t fileSize = static_cast<uint64_t>(endPos);

	uint64_t pos = 0;
	while (pos + 8 <= fileSize) {
		BoxHeader box;
		if (!ReadBoxHeader(fs, pos, fileSize, box))
			break;
		if (box.type == FourCC("moov")) {
			uint64_t mp = box.offset + box.headerSize;
			uint64_t mEnd = box.offset + box.size;
			while (mp + 8 <= mEnd) {
				BoxHeader child;
				if (!ReadBoxHeader(fs, mp, mEnd, child))
					break;
				if (child.type == FourCC("trak"))
					ParseTrak(fs, child.offset + child.headerSize, child.offset + child.size,
						  names);
				mp += child.size;
			}
			break;
		}
		pos += box.size;
	}
	return names;
}

} // namespace overlay_mp4
