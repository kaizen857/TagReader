#include "Id3Frames.hpp"

#include "cover/CoverCache.hpp"
#include "io/ByteReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using tagreader_core::Id3TagView;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_io::IsValidSyncSafe32;
using tagreader_io::ReadBE24;
using tagreader_io::ReadBE32;
using tagreader_io::ReadRange;
using tagreader_io::ReadSyncSafe32;
using tagreader_io::TryAddSize;
using tagreader_io::TryAddUintmax;
using tagreader_text::ReadId3ByteString;
using tagreader_text::ReadLatin1Text;
using tagreader_text::ReadLyricsFromPlainText;
using tagreader_text::TrimText;
using tagreader_cover::WriteCoverAsPng;

constexpr std::size_t kMaxId3TagBytes = 16z * 1024 * 1024;
constexpr std::size_t kMaxLyricLines = 20000;
constexpr std::size_t kMaxDecodedTextBytes = 2z * 1024 * 1024;
constexpr std::size_t kId3ResyncScanBudget = 4096;

uint16_t ParseUInt16(const std::string &value)
{
    const std::string trimmed = TrimText(value);
    if (trimmed.empty())
    {
        return 0;
    }

    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(trimmed, &consumed, 10);
        if (consumed != trimmed.size())
        {
            return 0;
        }
        if (parsed > std::numeric_limits<uint16_t>::max())
        {
            return 0;
        }
        return static_cast<uint16_t>(parsed);
    }
    catch (...)
    {
        return 0;
    }
}

uint16_t ParseYearOnly(std::string_view text)
{
    while (!text.empty())
    {
        const unsigned char ch = static_cast<unsigned char>(text.front());
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        {
            text.remove_prefix(1);
            continue;
        }
        break;
    }

    if (text.size() < 4)
    {
        return 0;
    }

    if (!std::isdigit(static_cast<unsigned char>(text[0])) || !std::isdigit(static_cast<unsigned char>(text[1])) || !std::isdigit(static_cast<unsigned char>(text[2])) || !std::isdigit(static_cast<unsigned char>(text[3])))
    {
        return 0;
    }

    if (text.size() > 4)
    {
        const unsigned char next = static_cast<unsigned char>(text[4]);
        if (std::isdigit(next))
        {
            return 0;
        }

        const bool allowedSeparator = next == '-' || next == '/' || next == '.' || next == ' ' || next == 'T' || next == '\0';
        if (!allowedSeparator)
        {
            return 0;
        }
    }

    const uint16_t year = static_cast<uint16_t>((text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0'));
    return (year >= 1000 && year <= 9999) ? year : 0;
}

std::pair<uint16_t, uint16_t> ParseSlashNumber(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos)
    {
        return {ParseUInt16(value), 0};
    }

    const std::string left = TrimText(value.substr(0, slash));
    const std::string right = TrimText(value.substr(slash + 1));
    if (left.empty() || right.empty())
    {
        return {0, 0};
    }

    const uint16_t current = ParseUInt16(left);
    const uint16_t total = ParseUInt16(right);
    if (current == 0 || total == 0)
    {
        return {0, 0};
    }

    return {current, total};
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool IsLikelyId3FrameId(std::string_view frameId)
{
    if (frameId.size() != 4)
    {
        return false;
    }

    return std::all_of(frameId.begin(), frameId.end(), [](unsigned char ch)
                       { return std::isalnum(ch) != 0 || ch == '_'; });
}

bool IsLikelyId3v22FrameId(std::string_view frameId)
{
    if (frameId.size() != 3)
    {
        return false;
    }

    return std::all_of(frameId.begin(), frameId.end(), [](unsigned char ch)
                       { return std::isalnum(ch) != 0 || ch == '_'; });
}

bool IsAllZeroFrom(const std::vector<uint8_t> &tagBytes, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (cursor >= limit)
    {
        return true;
    }

    return std::all_of(tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                       tagBytes.begin() + static_cast<std::ptrdiff_t>(limit),
                       [](uint8_t byte)
                       { return byte == 0; });
}

bool IsId3PaddingStartAtOriginalCursor(const std::vector<uint8_t> &tagBytes, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (cursor >= limit)
    {
        return true;
    }
    return tagBytes[cursor] == 0 || IsAllZeroFrom(tagBytes, cursor, limit);
}

bool IsValidId3v22FrameHeaderAt(const std::vector<uint8_t> &tagBytes, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (cursor + 6 > limit)
    {
        return false;
    }

    const uint8_t *frameHeader = tagBytes.data() + cursor;
    const std::string_view frameId(reinterpret_cast<const char *>(frameHeader), 3);
    if (!IsLikelyId3v22FrameId(frameId))
    {
        return false;
    }

    const uint32_t frameSize = ReadBE24(frameHeader + 3);
    return frameSize != 0 && frameSize <= limit - cursor - 6;
}

bool IsValidId3v23Or24FrameHeaderAt(const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (cursor + 10 > limit)
    {
        return false;
    }

    const uint8_t *frameHeader = tagBytes.data() + cursor;
    const std::string_view frameId(reinterpret_cast<const char *>(frameHeader), 4);
    if (!IsLikelyId3FrameId(frameId))
    {
        return false;
    }

    uint32_t frameSize = 0;
    if (versionMajor >= 4)
    {
        if (!IsValidSyncSafe32(frameHeader + 4))
        {
            return false;
        }
        frameSize = ReadSyncSafe32(frameHeader + 4);
    }
    else
    {
        frameSize = ReadBE32(frameHeader + 4);
    }

    return frameSize != 0 && frameSize <= limit - cursor - 10;
}

bool TryResyncId3v22Frame(const std::vector<uint8_t> &tagBytes, std::size_t &cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (IsId3PaddingStartAtOriginalCursor(tagBytes, cursor, limit))
    {
        return false;
    }

    for (std::size_t candidate = cursor + 1; candidate + 6 <= limit; ++candidate)
    {
        if (IsAllZeroFrom(tagBytes, candidate, limit))
        {
            return false;
        }
        if (IsValidId3v22FrameHeaderAt(tagBytes, candidate, limit))
        {
            cursor = candidate;
            return true;
        }
    }

    return false;
}

bool TryResyncId3v23Or24Frame(const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, std::size_t &cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    if (IsId3PaddingStartAtOriginalCursor(tagBytes, cursor, limit))
    {
        return false;
    }

    for (std::size_t candidate = cursor + 1; candidate + 10 <= limit; ++candidate)
    {
        if (IsAllZeroFrom(tagBytes, candidate, limit))
        {
            return false;
        }
        if (IsValidId3v23Or24FrameHeaderAt(tagBytes, versionMajor, candidate, limit))
        {
            cursor = candidate;
            return true;
        }
    }

    return false;
}

bool IsId3v22SupportedTextFrame(std::string_view frameId)
{
    return frameId == "TT2" || frameId == "TP1" || frameId == "TAL" || frameId == "TP2" ||
           frameId == "TCM" || frameId == "TCO" || frameId == "TYE" || frameId == "TRK" ||
           frameId == "TPA";
}

bool IsId3v23Or24SupportedTextFrame(std::string_view frameId)
{
    return frameId == "TIT2" || frameId == "TPE1" || frameId == "TALB" || frameId == "TPE2" ||
           frameId == "TCOM" || frameId == "TCON" || frameId == "TDRC" || frameId == "TYER" ||
           frameId == "TRCK" || frameId == "TPOS";
}

bool IsId3PictureFrame(std::string_view frameId)
{
    return frameId == "PIC" || frameId == "APIC";
}

bool IsId3LyricsFrame(std::string_view frameId)
{
    return frameId == "ULT" || frameId == "SLT" || frameId == "USLT" || frameId == "SYLT" ||
           frameId == "TXXX";
}

bool ShouldProcessId3v22MetadataFrame(std::string_view frameId)
{
    if (IsId3LyricsFrame(frameId))
    {
        return false;
    }

    return IsId3PictureFrame(frameId) || IsId3v22SupportedTextFrame(frameId);
}

bool ShouldProcessId3v23Or24MetadataFrame(std::string_view frameId)
{
    if (IsId3LyricsFrame(frameId))
    {
        return false;
    }

    return IsId3PictureFrame(frameId) || IsId3v23Or24SupportedTextFrame(frameId);
}

bool Id3v22TagFlagsAreSupported(uint8_t tagFlags)
{
    constexpr uint8_t kUnsynchronization = 0x80;
    constexpr uint8_t kCompression = 0x40;
    constexpr uint8_t kKnownFlags = kUnsynchronization | kCompression;

    return (tagFlags & ~kKnownFlags) == 0 && (tagFlags & kCompression) == 0;
}

bool Id3v23Or24FrameDataIsUnsupported(uint8_t versionMajor, uint16_t frameFlags)
{
    if (versionMajor == 3)
    {
        constexpr uint16_t kUnsupportedFlags = 0x0080 | 0x0040;
        return (frameFlags & kUnsupportedFlags) != 0;
    }

    constexpr uint16_t kUnsupportedFlags = 0x0008 | 0x0004;
    return (frameFlags & kUnsupportedFlags) != 0;
}

bool Id3v24FrameHasUnsynchronization(uint16_t frameFlags)
{
    return (frameFlags & 0x0002) != 0;
}

bool Id3v24TagUnsyncAppliesToPayload(uint8_t versionMajor, bool tagUnsync, uint16_t frameFlags)
{
    return versionMajor == 4 && tagUnsync && !Id3v24FrameHasUnsynchronization(frameFlags);
}

bool Id3v23Or24FrameDataNeedsTransform(uint8_t versionMajor, uint16_t frameFlags, bool applyTagUnsync)
{
    if (versionMajor == 3)
    {
        return (frameFlags & 0x0020) != 0;
    }

    constexpr uint16_t kTransformFlags = 0x0040 | 0x0002 | 0x0001;
    return (frameFlags & kTransformFlags) != 0 || applyTagUnsync;
}

std::size_t FindEncodedTerminator(const uint8_t *data, std::size_t size, uint8_t encoding)
{
    if (encoding == 1 || encoding == 2)
    {
        for (std::size_t i = 0; i + 1 < size; i += 2)
        {
            if (data[i] == 0 && data[i + 1] == 0)
            {
                return i;
            }
        }
        return size;
    }

    for (std::size_t i = 0; i < size; ++i)
    {
        if (data[i] == 0)
        {
            return i;
        }
    }
    return size;
}

std::size_t EncodedTerminatorWidth(uint8_t encoding)
{
    return (encoding == 1 || encoding == 2) ? 2U : 1U;
}

std::vector<uint8_t> RemoveId3Unsynchronization(std::vector<uint8_t> bytes)
{
    std::vector<uint8_t> result;
    result.reserve(bytes.size());

    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        result.push_back(bytes[i]);
        if (bytes[i] == 0xFF && i + 1 < bytes.size() && bytes[i + 1] == 0x00)
        {
            ++i;
        }
    }

    return result;
}

bool PrepareId3v23Or24FrameData(uint8_t versionMajor, uint16_t frameFlags, bool applyTagUnsync, std::vector<uint8_t> &frameData)
{
    std::size_t payloadCursor = 0;
    bool hasFrameUnsynchronization = false;
    bool hasDataLengthIndicator = false;
    uint32_t declaredSize = 0;

    if (versionMajor == 3)
    {
        const bool hasCompression = (frameFlags & 0x0080) != 0;
        const bool hasEncryption = (frameFlags & 0x0040) != 0;
        const bool hasGroupingIdentity = (frameFlags & 0x0020) != 0;
        if (hasCompression || hasEncryption)
        {
            return false;
        }
        if (hasGroupingIdentity)
        {
            if (frameData.size() < 1)
            {
                return false;
            }
            payloadCursor += 1;
        }
    }
    else
    {
        const bool hasGroupingIdentity = (frameFlags & 0x0040) != 0;
        const bool hasCompression = (frameFlags & 0x0008) != 0;
        const bool hasEncryption = (frameFlags & 0x0004) != 0;
        hasFrameUnsynchronization = (frameFlags & 0x0002) != 0;
        hasDataLengthIndicator = (frameFlags & 0x0001) != 0;
        if (hasCompression || hasEncryption)
        {
            return false;
        }
        if (hasGroupingIdentity)
        {
            if (frameData.size() < 1)
            {
                return false;
            }
            payloadCursor += 1;
        }
        if (hasDataLengthIndicator)
        {
            if (frameData.size() - payloadCursor < 4 || !IsValidSyncSafe32(frameData.data() + payloadCursor))
            {
                return false;
            }

            declaredSize = ReadSyncSafe32(frameData.data() + payloadCursor);
            payloadCursor += 4;
            if (declaredSize > frameData.size() - payloadCursor)
            {
                return false;
            }
        }
    }

    if (payloadCursor > 0)
    {
        frameData.erase(frameData.begin(), frameData.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
    }

    if (hasDataLengthIndicator)
    {
        frameData.resize(declaredSize);
    }

    if (versionMajor == 4 && (hasFrameUnsynchronization || applyTagUnsync))
    {
        frameData = RemoveId3Unsynchronization(std::move(frameData));
    }

    return true;
}

bool PrepareId3v24FrameRegion(const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, uint8_t tagFlags, std::size_t &cursor, std::size_t &frameLimit)
{
    frameLimit = tagBytes.size();
    if (versionMajor == 4 && (tagFlags & 0x10) != 0)
    {
        // ID3v2.4 footer is part of tag size but must never be scanned as a frame.
        if (frameLimit < 10)
        {
            return false;
        }

        const std::size_t footerOffset = frameLimit - 10;
        const uint8_t *footer = tagBytes.data() + footerOffset;
        if (std::memcmp(footer, "3DI", 3) != 0 || footer[3] != 4 || !IsValidSyncSafe32(footer + 6))
        {
            return false;
        }
        frameLimit = footerOffset;
    }

    if (versionMajor == 4 && (tagFlags & 0x40) != 0)
    {
        if (frameLimit < 6 || !IsValidSyncSafe32(tagBytes.data()))
        {
            return false;
        }

        const std::size_t extSize = ReadSyncSafe32(tagBytes.data());
        const std::size_t extendedEnd = extSize;
        if (extSize < 6 || extendedEnd > frameLimit)
        {
            return false;
        }

        const uint8_t flagBytes = tagBytes[4];
        std::size_t flagEnd = 0;
        if (flagBytes == 0 || !TryAddSize(5, flagBytes, flagEnd) || flagEnd > extendedEnd)
        {
            return false;
        }

        cursor = extendedEnd;
    }

    return true;
}

bool PrepareId3v23ExtendedHeader(const std::vector<uint8_t> &tagBytes, std::size_t frameLimit, std::size_t &cursor)
{
    if (frameLimit < 4)
    {
        return false;
    }

    const std::size_t extSize = ReadBE32(tagBytes.data());
    std::size_t extendedEnd = 0;
    if (extSize < 6 || !TryAddSize(4, extSize, extendedEnd) || extendedEnd > frameLimit)
    {
        return false;
    }

    cursor = extendedEnd;
    return true;
}

std::string ReadId3TextFrame(const uint8_t *data, std::size_t size)
{
    if (size == 0)
    {
        return {};
    }

    const uint8_t encoding = data[0];
    const uint8_t *payload = data + 1;
    const std::size_t payloadSize = size - 1;

    return ReadId3ByteString(payload, payloadSize, encoding);
}

constexpr std::array<std::string_view, 192> Id3v1Genres{
    "Blues",
    "Classic Rock",
    "Country",
    "Dance",
    "Disco",
    "Funk",
    "Grunge",
    "Hip-Hop",
    "Jazz",
    "Metal",
    "New Age",
    "Oldies",
    "Other",
    "Pop",
    "R&B",
    "Rap",
    "Reggae",
    "Rock",
    "Techno",
    "Industrial",
    "Alternative",
    "Ska",
    "Death Metal",
    "Pranks",
    "Soundtrack",
    "Euro-Techno",
    "Ambient",
    "Trip-Hop",
    "Vocal",
    "Jazz+Funk",
    "Fusion",
    "Trance",
    "Classical",
    "Instrumental",
    "Acid",
    "House",
    "Game",
    "Sound Clip",
    "Gospel",
    "Noise",
    "AlternRock",
    "Bass",
    "Soul",
    "Punk",
    "Space",
    "Meditative",
    "Instrumental Pop",
    "Instrumental Rock",
    "Ethnic",
    "Gothic",
    "Darkwave",
    "Techno-Industrial",
    "Electronic",
    "Pop-Folk",
    "Eurodance",
    "Dream",
    "Southern Rock",
    "Comedy",
    "Cult",
    "Gangsta",
    "Top 40",
    "Christian Rap",
    "Pop/Funk",
    "Jungle",
    "Native American",
    "Cabaret",
    "New Wave",
    "Psychadelic",
    "Rave",
    "Showtunes",
    "Trailer",
    "Lo-Fi",
    "Tribal",
    "Acid Punk",
    "Acid Jazz",
    "Polka",
    "Retro",
    "Musical",
    "Rock & Roll",
    "Hard Rock",
    "Folk",
    "Folk-Rock",
    "National Folk",
    "Swing",
    "Fast Fusion",
    "Bebob",
    "Latin",
    "Revival",
    "Celtic",
    "Bluegrass",
    "Avantgarde",
    "Gothic Rock",
    "Progressive Rock",
    "Psychedelic Rock",
    "Symphonic Rock",
    "Slow Rock",
    "Big Band",
    "Chorus",
    "Easy Listening",
    "Acoustic",
    "Humour",
    "Speech",
    "Chanson",
    "Opera",
    "Chamber Music",
    "Sonata",
    "Symphony",
    "Booty Bass",
    "Primus",
    "Porn Groove",
    "Satire",
    "Slow Jam",
    "Club",
    "Tango",
    "Samba",
    "Folklore",
    "Ballad",
    "Power Ballad",
    "Rhythmic Soul",
    "Freestyle",
    "Duet",
    "Punk Rock",
    "Drum Solo",
    "A Cappella",
    "Euro-House",
    "Dance Hall",
    "Goa",
    "Drum & Bass",
    "Club-House",
    "Hardcore",
    "Terror",
    "Indie",
    "BritPop",
    "Negerpunk",
    "Polsk Punk",
    "Beat",
    "Christian Gangsta Rap",
    "Heavy Metal",
    "Black Metal",
    "Crossover",
    "Contemporary Christian",
    "Christian Rock",
    "Merengue",
    "Salsa",
    "Thrash Metal",
    "Anime",
    "JPop",
    "Synthpop",
    "Abstract",
    "Art Rock",
    "Baroque",
    "Bhangra",
    "Big Beat",
    "Breakbeat",
    "Chillout",
    "Downtempo",
    "Dub",
    "EBM",
    "Eclectic",
    "Electro",
    "Electroclash",
    "Emo",
    "Experimental",
    "Garage",
    "Global",
    "IDM",
    "Illbient",
    "Industro-Goth",
    "Jam Band",
    "Krautrock",
    "Leftfield",
    "Lounge",
    "Math Rock",
    "New Romantic",
    "Nu-Breakz",
    "Post-Punk",
    "Post-Rock",
    "Psytrance",
    "Shoegaze",
    "Space Rock",
    "Trop Rock",
    "World Music",
    "Neoclassical",
    "Audiobook",
    "Audio Theatre",
    "Neue Deutsche Welle",
    "Podcast",
    "Indie Rock",
    "G-Funk",
    "Dubstep",
    "Garage Rock",
    "Psybient",
};

std::string NormalizeId3Genre(std::string_view value)
{
    auto genreFromIndex = [](std::string_view digits) -> std::string
    {
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch)
                                           { return std::isdigit(ch) != 0; }))
        {
            return {};
        }
        const uint16_t index = ParseUInt16(std::string(digits));
        if (index < Id3v1Genres.size())
        {
            return std::string(Id3v1Genres[index]);
        }
        return {};
    };

    auto normalizeOne = [&](std::string_view raw) -> std::string
    {
        std::string text = TrimText(std::string(raw));
        if (text.empty())
        {
            return {};
        }

        if (text.front() == '(')
        {
            const auto close = text.find(')');
            if (close != std::string::npos)
            {
                const std::string mapped = genreFromIndex(std::string_view(text).substr(1, close - 1));
                const std::string suffix = TrimText(text.substr(close + 1));
                if (!suffix.empty())
                {
                    return suffix;
                }
                if (!mapped.empty())
                {
                    return mapped;
                }
            }
        }

        const std::string mapped = genreFromIndex(text);
        return mapped.empty() ? text : mapped;
    };

    std::size_t start = 0;
    while (start <= value.size())
    {
        const std::size_t end = value.find('\0', start);
        const std::string normalized = normalizeOne(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!normalized.empty())
        {
            return normalized;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }

    return {};
}


std::optional<std::string_view> LookupGenre(unsigned char genreIndex)
{
    if (genreIndex < Id3v1Genres.size())
    {
        return Id3v1Genres[genreIndex];
    }
    return std::nullopt;
}
} // namespace

namespace tagreader_id3
{
void ReadID3v22Frame(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize);
void ReadID3v22PictureFrame(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize);
void ReadID3v2Frame(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize);
void ReadID3v2PictureFrame(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize);
void ReadID3v2ApicPayload(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, std::string_view mimeType, uint8_t pictureType, const uint8_t *imageData, std::size_t imageSize);

std::optional<std::string_view> LookupId3v1Genre(unsigned char genreIndex)
{
    return LookupGenre(genreIndex);
}

bool Id3v22TagFlagsAreSupported(uint8_t tagFlags)
{
    return ::Id3v22TagFlagsAreSupported(tagFlags);
}

bool ReadId3TagBytes(ReadContext &context, Id3TagView &tagView)
{
    tagView = {};
    if (!context.input.is_open())
    {
        return false;
    }

    const std::vector<uint8_t> header = ReadRange(context.input, 0, 10);
    if (header.size() != 10 || std::memcmp(header.data(), "ID3", 3) != 0)
    {
        return false;
    }

    const uint8_t versionMajor = header[3];
    const uint8_t flags = header[5];
    if (versionMajor < 2 || versionMajor > 4)
    {
        return false;
    }
    if (!IsValidSyncSafe32(header.data() + 6))
    {
        return false;
    }

    const uint32_t tagSize = ReadSyncSafe32(header.data() + 6);
    if (tagSize > kMaxId3TagBytes)
    {
        return false;
    }

    std::uintmax_t tagEnd = 0;
    if (!TryAddUintmax(10, tagSize, tagEnd) || tagEnd > context.fileSize)
    {
        return false;
    }

    std::vector<uint8_t> tagBytes = ReadRange(context.input, 10, tagSize, kMaxId3TagBytes);
    if (tagBytes.size() != tagSize)
    {
        return false;
    }
    const bool tagUnsync = (flags & 0x80) != 0;

    std::size_t cursor = 0;
    std::size_t frameLimit = tagBytes.size();
    if (!PrepareId3v24FrameRegion(tagBytes, versionMajor, flags, cursor, frameLimit))
    {
        return false;
    }

    if (versionMajor < 4 && tagUnsync)
    {
        tagBytes = RemoveId3Unsynchronization(std::move(tagBytes));
        frameLimit = tagBytes.size();
    }

    if (versionMajor >= 3 && (flags & 0x40) != 0)
    {
        if (versionMajor == 3)
        {
            if (!PrepareId3v23ExtendedHeader(tagBytes, frameLimit, cursor))
            {
                return false;
            }
        }
        else if (versionMajor == 4)
        {
            // v2.4 extended header / footer region has already been validated above.
        }
        else
        {
            return false;
        }
    }

    tagView.versionMajor = versionMajor;
    tagView.flags = flags;
    tagView.tagUnsync = versionMajor == 4 && tagUnsync;
    tagView.cursor = cursor;
    tagView.limit = frameLimit;
    tagView.bytes = std::move(tagBytes);
    return true;
}

void ReadID3v22Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, std::size_t cursor)
{
    while (cursor + 6 <= tagBytes.size())
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 3);
        if (!IsLikelyId3v22FrameId(frameId))
        {
            if (TryResyncId3v22Frame(tagBytes, cursor, std::min(tagBytes.size(), cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }

        const uint32_t frameSize = ReadBE24(frameHeader + 3);
        if (frameSize == 0)
        {
            if (TryResyncId3v22Frame(tagBytes, cursor, std::min(tagBytes.size(), cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }
        if (cursor + 6 + frameSize > tagBytes.size())
        {
            if (TryResyncId3v22Frame(tagBytes, cursor, std::min(tagBytes.size(), cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }

        const uint8_t *frameData = tagBytes.data() + cursor + 6;
        ReadID3v22Frame(context, metadata, frameId, frameData, frameSize);

        cursor += 6 + static_cast<std::size_t>(frameSize);
    }
}

void ReadID3v23Or24Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    while (cursor + 10 <= limit)
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 4);
        if (!IsLikelyId3FrameId(frameId))
        {
            if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, std::min(limit, cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }

        uint32_t frameSize = 0;
        if (versionMajor >= 4)
        {
            if (!IsValidSyncSafe32(frameHeader + 4))
            {
                if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, std::min(limit, cursor + kId3ResyncScanBudget)))
                {
                    continue;
                }
                break;
            }
            frameSize = ReadSyncSafe32(frameHeader + 4);
        }
        else
        {
            frameSize = ReadBE32(frameHeader + 4);
        }

        if (frameSize == 0)
        {
            if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, std::min(limit, cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }
        if (frameSize > limit - cursor - 10)
        {
            if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, std::min(limit, cursor + kId3ResyncScanBudget)))
            {
                continue;
            }
            break;
        }

        const std::size_t frameDataOffset = cursor + 10;
        const std::size_t frameEnd = frameDataOffset + static_cast<std::size_t>(frameSize);
        if (!ShouldProcessId3v23Or24MetadataFrame(frameId))
        {
            cursor = frameEnd;
            continue;
        }

        const uint16_t frameFlags = static_cast<uint16_t>((frameHeader[8] << 8) | frameHeader[9]);
        if (Id3v23Or24FrameDataIsUnsupported(versionMajor, frameFlags))
        {
            cursor = frameEnd;
            continue;
        }

        const uint8_t *frameData = tagBytes.data() + frameDataOffset;
        std::size_t frameDataSize = frameSize;
        std::vector<uint8_t> transformedFrameData;
        const bool applyTagUnsync = Id3v24TagUnsyncAppliesToPayload(versionMajor, tagUnsync, frameFlags);
        if (Id3v23Or24FrameDataNeedsTransform(versionMajor, frameFlags, applyTagUnsync))
        {
            transformedFrameData.assign(tagBytes.begin() + static_cast<std::ptrdiff_t>(frameDataOffset),
                                        tagBytes.begin() + static_cast<std::ptrdiff_t>(frameEnd));
            if (!PrepareId3v23Or24FrameData(versionMajor, frameFlags, applyTagUnsync, transformedFrameData))
            {
                cursor = frameEnd;
                continue;
            }
            frameData = transformedFrameData.data();
            frameDataSize = transformedFrameData.size();
        }

        ReadID3v2Frame(context, metadata, frameId, frameData, frameDataSize);

        cursor = frameEnd;
    }
}

void ReadID3v22Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize)
{
    if (!ShouldProcessId3v22MetadataFrame(frameId))
    {
        return;
    }
    if (IsId3PictureFrame(frameId))
    {
        ReadID3v22PictureFrame(context, metadata, frameData, frameSize);
        return;
    }
    if (!IsId3v22SupportedTextFrame(frameId))
    {
        return;
    }

    const std::string value = ReadId3TextFrame(frameData, frameSize);
    if (value.empty())
    {
        return;
    }

    if (frameId == "TT2")
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (frameId == "TP1")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (frameId == "TAL")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (frameId == "TP2")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (frameId == "TCM")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (frameId == "TCO")
    {
        if (metadata.genre.empty())
            metadata.genre = NormalizeId3Genre(value);
    }
    else if (frameId == "TYE")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (frameId == "TRK")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (frameId == "TPA")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void ReadID3v22PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameData == nullptr || frameSize < 6)
    {
        return;
    }

    const uint8_t encoding = frameData[0];
    const std::string imageFormat(reinterpret_cast<const char *>(frameData + 1), 3);
    const uint8_t pictureType = frameData[4];
    const uint8_t *payload = frameData + 5;
    const std::size_t payloadSize = frameSize - 5;
    if (pictureType != 3)
    {
        return;
    }

    // ID3v2.2 PIC stores description immediately after picture type; image bytes start after its terminator.
    const std::size_t descSize = FindEncodedTerminator(payload, payloadSize, encoding);
    if (descSize >= payloadSize)
    {
        return;
    }
    const std::size_t cursor = descSize + EncodedTerminatorWidth(encoding);
    if (cursor >= payloadSize)
    {
        return;
    }

    (void)imageFormat;
    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, payload + cursor, payloadSize - cursor);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}

void ReadID3v2Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize)
{
    if (!ShouldProcessId3v23Or24MetadataFrame(frameId))
    {
        return;
    }
    if (IsId3PictureFrame(frameId))
    {
        ReadID3v2PictureFrame(context, metadata, frameData, frameSize);
        return;
    }
    if (!IsId3v23Or24SupportedTextFrame(frameId))
    {
        return;
    }

    const std::string value = ReadId3TextFrame(frameData, frameSize);
    if (value.empty())
    {
        return;
    }

    if (frameId == "TIT2")
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (frameId == "TPE1")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (frameId == "TALB")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (frameId == "TPE2")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (frameId == "TCOM")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (frameId == "TCON")
    {
        if (metadata.genre.empty())
            metadata.genre = NormalizeId3Genre(value);
    }
    else if (frameId == "TYER" || frameId == "TDRC")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (frameId == "TRCK")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (frameId == "TPOS")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void ReadID3v2PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameData == nullptr || frameSize < 4)
    {
        return;
    }

    const uint8_t encoding = frameData[0];
    const uint8_t *payload = frameData + 1;
    const std::size_t payloadSize = frameSize - 1;

    std::size_t cursor = 0;
    while (cursor < payloadSize && payload[cursor] != 0)
    {
        ++cursor;
    }
    if (cursor >= payloadSize)
    {
        return;
    }

    const std::string mimeType = ReadLatin1Text(payload, cursor);
    ++cursor;
    if (cursor >= payloadSize)
    {
        return;
    }

    const uint8_t pictureType = payload[cursor];
    if (pictureType != 3)
    {
        return;
    }
    ++cursor;
    if (cursor >= payloadSize)
    {
        return;
    }
    const std::size_t descSize = FindEncodedTerminator(payload + cursor, payloadSize - cursor, encoding);
    if (descSize >= payloadSize - cursor)
    {
        return;
    }
    cursor += descSize + EncodedTerminatorWidth(encoding);
    if (cursor >= payloadSize)
    {
        return;
    }

    ReadID3v2ApicPayload(context, metadata, mimeType, pictureType, payload + cursor, payloadSize - cursor);
}

void ReadID3v2ApicPayload(ReadContext &context, RawMetadata &metadata, std::string_view mimeType, uint8_t pictureType, const uint8_t *imageData, std::size_t imageSize)
{
    if (pictureType != 3 || imageData == nullptr || imageSize == 0)
    {
        return;
    }

    (void)mimeType;
    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, imageData, imageSize);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}

void ReadID3v22LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, std::size_t cursor)
{
    (void)context;
    RawLyrics ultCandidate{};
    RawLyrics sltCandidate{};

    while (cursor + 6 <= tagBytes.size())
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 3);
        if (!IsLikelyId3v22FrameId(frameId))
        {
            if (TryResyncId3v22Frame(tagBytes, cursor, tagBytes.size()))
            {
                continue;
            }
            break;
        }

        const uint32_t frameSize = ReadBE24(frameHeader + 3);
        if (frameSize == 0 || cursor + 6 + frameSize > tagBytes.size())
        {
            if (TryResyncId3v22Frame(tagBytes, cursor, tagBytes.size()))
            {
                continue;
            }
            break;
        }

        const uint8_t *frameData = tagBytes.data() + cursor + 6;
        if (frameId == "ULT" && frameSize > 4)
        {
            std::size_t p = 0;
            const uint8_t encoding = frameData[p++];
            p += 3;
            const std::size_t descSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
            if (descSize < frameSize - p)
            {
                p += descSize + EncodedTerminatorWidth(encoding);
                if (p < frameSize)
                {
                    ReadLyricsFromPlainText(ultCandidate, ReadId3ByteString(frameData + p, frameSize - p, encoding));
                }
            }
        }
        else if (frameId == "SLT" && frameSize > 6 && sltCandidate.timedLines.empty())
        {
            const uint8_t encoding = frameData[0];
            const uint8_t timestampFormat = frameData[4];
            const uint8_t contentType = frameData[5];
            (void)contentType;
            if (timestampFormat == 2)
            {
                std::vector<std::pair<std::chrono::microseconds, std::string>> timedLines;

                std::size_t p = 1 + 3 + 1 + 1;
                const std::size_t descriptorSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
                if (descriptorSize < frameSize - p)
                {
                    p += descriptorSize + EncodedTerminatorWidth(encoding);
                    while (p < frameSize && timedLines.size() < kMaxLyricLines)
                    {
                        const std::size_t textStart = p;
                        const std::size_t lineSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
                        if (lineSize >= frameSize - p)
                        {
                            break;
                        }

                        const std::string line = ReadId3ByteString(frameData + textStart, lineSize, encoding);
                        p += lineSize + EncodedTerminatorWidth(encoding);
                        if (p + 4 > frameSize)
                        {
                            break;
                        }

                        const uint32_t timestampMs = ReadBE32(frameData + p);
                        p += 4;
                        std::string normalizedLine = TrimText(line);
                        if (!normalizedLine.empty())
                        {
                            timedLines.emplace_back(std::chrono::microseconds(static_cast<int64_t>(timestampMs) * 1000), std::move(normalizedLine));
                        }
                    }

                    if (!timedLines.empty())
                    {
                        sltCandidate.timedLines = std::move(timedLines);
                    }
                }
            }
        }

        cursor += 6 + static_cast<std::size_t>(frameSize);
    }

    if (!lyrics.text.empty() || !lyrics.timedLines.empty())
    {
        return;
    }
    if (!sltCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(sltCandidate.timedLines);
    }
    else if (!ultCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(ultCandidate.timedLines);
    }
    else if (!ultCandidate.text.empty())
    {
        lyrics.text = std::move(ultCandidate.text);
    }
}

void ReadID3v23Or24LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit)
{
    RawLyrics usltCandidate{};
    RawLyrics syltCandidate{};
    RawLyrics txxxCandidate{};

    limit = std::min(limit, tagBytes.size());
    while (cursor + 10 <= limit)
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 4);
        if (!IsLikelyId3FrameId(frameId))
        {
            if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, limit))
            {
                continue;
            }
            break;
        }

        uint32_t frameSize = 0;
        if (versionMajor >= 4)
        {
            if (!IsValidSyncSafe32(frameHeader + 4))
            {
                if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, limit))
                {
                    continue;
                }
                break;
            }
            frameSize = ReadSyncSafe32(frameHeader + 4);
        }
        else
        {
            frameSize = ReadBE32(frameHeader + 4);
        }
        if (frameSize == 0 || frameSize > limit - cursor - 10)
        {
            if (TryResyncId3v23Or24Frame(tagBytes, versionMajor, cursor, limit))
            {
                continue;
            }
            break;
        }

        const uint16_t frameFlags = static_cast<uint16_t>((frameHeader[8] << 8) | frameHeader[9]);
        std::vector<uint8_t> frameData(tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10),
                                       tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10 + frameSize));
        const bool applyTagUnsync = Id3v24TagUnsyncAppliesToPayload(versionMajor, tagUnsync, frameFlags);
        if (!PrepareId3v23Or24FrameData(versionMajor, frameFlags, applyTagUnsync, frameData))
        {
            cursor += 10 + static_cast<std::size_t>(frameSize);
            continue;
        }

        if (frameId == "USLT")
        {
            if (usltCandidate.text.empty() && frameData.size() > 4)
            {
                std::size_t p = 0;
                const uint8_t encoding = frameData[p++];
                p += 3;
                const std::size_t descSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                if (descSize >= frameData.size() - p)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }
                p += descSize + EncodedTerminatorWidth(encoding);
                std::string text;
                if (p < frameData.size())
                {
                    text = ReadId3ByteString(frameData.data() + p, frameData.size() - p, encoding);
                }
                ReadLyricsFromPlainText(usltCandidate, text);
            }
        }
        else if (frameId == "SYLT")
        {
            if (syltCandidate.timedLines.empty() && frameData.size() > 6)
            {
                const uint8_t encoding = frameData[0];
                const uint8_t timestampFormat = frameData[4];
                const uint8_t contentType = frameData[5];
                (void)contentType;
                if (timestampFormat != 2)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }

                std::vector<std::pair<std::chrono::microseconds, std::string>> timedLines;

                std::size_t p = 1 + 3 + 1 + 1;
                const std::size_t descriptorSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                if (descriptorSize >= frameData.size() - p)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }
                p += descriptorSize + EncodedTerminatorWidth(encoding);
                if (p >= frameData.size())
                {
                    break;
                }

                while (p < frameData.size() && timedLines.size() < kMaxLyricLines)
                {
                    const std::size_t textStart = p;
                    const std::size_t lineSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                    if (lineSize >= frameData.size() - p)
                    {
                        break;
                    }
                    const std::string line = ReadId3ByteString(frameData.data() + textStart, lineSize, encoding);
                    p += lineSize + EncodedTerminatorWidth(encoding);
                    if (p + 4 > frameData.size())
                    {
                        break;
                    }
                    const uint32_t timestampMs = ReadBE32(frameData.data() + p);
                    p += 4;
                    std::string normalizedLine = TrimText(line);
                    if (!normalizedLine.empty())
                    {
                        timedLines.emplace_back(std::chrono::microseconds(static_cast<int64_t>(timestampMs) * 1000), std::move(normalizedLine));
                    }
                }

                if (!timedLines.empty())
                {
                    syltCandidate.timedLines = std::move(timedLines);
                }
            }
        }
        else if (frameId == "TXXX")
        {
            if (txxxCandidate.text.empty() && frameData.size() > 1)
            {
                const uint8_t encoding = frameData[0];
                const uint8_t *payload = frameData.data() + 1;
                const std::size_t payloadSize = frameData.size() - 1;

                const std::size_t descSize = FindEncodedTerminator(payload, payloadSize, encoding);
                if (descSize >= payloadSize)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }

                const std::string description = ReadId3ByteString(payload, descSize, encoding);
                const std::string lowerDescription = ToLower(description);
                if (lowerDescription == "lyrics" || lowerDescription == "unsyncedlyrics" || lowerDescription == "lyric" || lowerDescription == "sylt" || lowerDescription == "syncedlyrics")
                {
                    const std::size_t valueOffset = descSize + EncodedTerminatorWidth(encoding);
                    if (valueOffset < payloadSize)
                    {
                        const std::string value = ReadId3ByteString(payload + valueOffset, payloadSize - valueOffset, encoding);
                        ReadLyricsFromPlainText(txxxCandidate, value);
                    }
                }
            }
        }

        cursor += 10 + static_cast<std::size_t>(frameSize);
    }

    if (!lyrics.text.empty() || !lyrics.timedLines.empty())
    {
        return;
    }
    if (!usltCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(usltCandidate.timedLines);
    }
    else if (!usltCandidate.text.empty())
    {
        lyrics.text = std::move(usltCandidate.text);
    }
    else if (!syltCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(syltCandidate.timedLines);
    }
    else if (!txxxCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(txxxCandidate.timedLines);
    }
    else if (!txxxCandidate.text.empty())
    {
        lyrics.text = std::move(txxxCandidate.text);
    }
}


} // namespace tagreader_id3
