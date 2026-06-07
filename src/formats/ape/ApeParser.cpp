#include "formats/ape/ApeLimits.hpp"
#include "formats/ape/ApeParser.hpp"

#include "cover/CoverCache.hpp"
#include "io/ByteReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace
{
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_cover::WriteCoverAsPng;
using tagreader_io::ReadLE32;
using tagreader_io::ReadRange;
using tagreader_text::ReadLyricsFromPlainText;
using tagreader_text::ReadUtf8Text;

bool IEquals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char ca, unsigned char cb) { return std::tolower(ca) == std::tolower(cb); });
}

uint16_t ParseUInt16(const std::string &value)
{
    if (value.empty())
    {
        return 0;
    }

    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed == 0)
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

    if (!std::isdigit(static_cast<unsigned char>(text[0])) ||
        !std::isdigit(static_cast<unsigned char>(text[1])) ||
        !std::isdigit(static_cast<unsigned char>(text[2])) ||
        !std::isdigit(static_cast<unsigned char>(text[3])))
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

        const bool allowedSeparator = next == '-' || next == '/' || next == '.' ||
                                      next == ' ' || next == 'T' || next == '\0';
        if (!allowedSeparator)
        {
            return 0;
        }
    }

    const uint16_t year = static_cast<uint16_t>(
        (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
        (text[2] - '0') * 10 + (text[3] - '0'));
    return (year >= 1000 && year <= 9999) ? year : 0;
}

std::pair<uint16_t, uint16_t> ParseSlashNumber(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos)
    {
        return {ParseUInt16(value), 0};
    }
    return {ParseUInt16(value.substr(0, slash)), ParseUInt16(value.substr(slash + 1))};
}

bool FindApeFooter(ReadContext &context, uint32_t &version, uint32_t &tagSize,
                   uint32_t &itemCount, uint32_t &flags)
{
    if (context.fileSize < 32)
    {
        return false;
    }

    const std::vector<uint8_t> footerBytes = ReadRange(context.input, context.fileSize - 32, 32);
    if (footerBytes.size() != 32)
    {
        return false;
    }
    if (std::memcmp(footerBytes.data(), "APETAGEX", 8) != 0)
    {
        return false;
    }

    const uint8_t *footer = footerBytes.data();
    version = ReadLE32(footer + 8);
    tagSize = ReadLE32(footer + 12);
    itemCount = ReadLE32(footer + 16);
    flags = ReadLE32(footer + 20);

    return true;
}

void ProcessApeTextItem(RawMetadata &metadata, std::string_view key, const uint8_t *valueData,
                        uint32_t valueSize)
{
    if (valueData == nullptr || valueSize == 0 || valueSize > tagreader_ape::kMaxApeItemValueBytes)
    {
        return;
    }

    const std::string value = ReadUtf8Text(valueData, valueSize);
    if (value.empty())
    {
        return;
    }

    if (IEquals(key, "title"))
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (IEquals(key, "artist"))
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (IEquals(key, "album"))
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (IEquals(key, "album artist"))
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (IEquals(key, "composer"))
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (IEquals(key, "genre"))
    {
        if (metadata.genre.empty())
            metadata.genre = value;
    }
    else if (IEquals(key, "year"))
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (IEquals(key, "track") || IEquals(key, "tracknumber"))
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (IEquals(key, "disc") || IEquals(key, "discnumber"))
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void ProcessApeCoverItem(ReadContext &context, RawMetadata &metadata,
                         const uint8_t *valueData, uint32_t valueSize)
{
    if (valueData == nullptr || valueSize == 0)
    {
        return;
    }

    constexpr std::size_t kMaxApeCoverItemBytes = 8z * 1024 * 1024; // 8 MiB
    if (valueSize > kMaxApeCoverItemBytes)
    {
        return;  // silently skip oversized cover item
    }

    if (!metadata.coverPath.empty())
    {
        return;
    }

    const uint8_t *imageData = valueData;
    std::size_t imageSize = valueSize;

    // Handle optional description prefix per APE spec:
    // If first byte is 0x00, find the second 0x00 as separator.
    // After the separator is the raw image data.
    if (valueSize > 1 && valueData[0] == 0x00)
    {
        std::size_t separatorPos = 1;
        while (separatorPos < valueSize && valueData[separatorPos] != 0x00)
        {
            ++separatorPos;
        }
        if (separatorPos + 1 < valueSize)
        {
            // Found a zero-length description (0x00 0x00) or a named description
            imageData = valueData + separatorPos + 1;
            imageSize = valueSize - (separatorPos + 1);
        }
    }

    if (imageSize == 0)
    {
        return;
    }

    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, imageData, imageSize);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}

} // anonymous namespace

namespace tagreader_ape
{

void ReadApeMetadata(ReadContext &context, RawMetadata &metadata)
{
    if (context.fileSize < 32)
    {
        return;
    }

    uint32_t version = 0;
    uint32_t tagSize = 0;
    uint32_t itemCount = 0;
    uint32_t flags = 0;

    if (!FindApeFooter(context, version, tagSize, itemCount, flags))
    {
        return;
    }

    // APEv1 tags (version < 2000) are unsupported — skip silently
    if (version < 2000)
    {
        return;
    }

    if (tagSize > kMaxApeTagBytes)
    {
        throw std::runtime_error("APE tag exceeds resource limits (tagSize)");
    }
    if (itemCount > kMaxApeItems)
    {
        throw std::runtime_error("APE tag exceeds resource limits (itemCount)");
    }

    const bool hasHeader = (flags & 0x80000000) != 0;

    // Guard against unsigned subtraction wrap when tagSize > fileSize.
    // The 32-byte footer is always at EOF; without header items sit before it,
    // so tagSize must not exceed fileSize - 32 in either path.
    // Without this check, fileSize - tagSize wraps to a huge uint64_t value.
    if (tagSize > context.fileSize - 32)
    {
        return;
    }

    // With header: items start at fileSize - tagSize (tagSize includes both header and items).
    // Without header: items are right before the 32-byte footer, so start at fileSize - 32 - tagSize.
    const uint64_t itemRegionOffset = hasHeader
        ? context.fileSize - static_cast<uint64_t>(tagSize)
        : context.fileSize - 32 - static_cast<uint64_t>(tagSize);

    const std::vector<uint8_t> itemBytes =
        ReadRange(context.input, itemRegionOffset, tagSize, kMaxApeTagBytes);
    if (itemBytes.size() < tagSize)
    {
        return;
    }

    size_t cursor = 0;
    for (uint32_t i = 0; i < itemCount; ++i)
    {
        // Minimum item framing: 4B valueSize + 4B flags + at least 1B key + 1B NUL = 10 bytes
        if (cursor + 10 > itemBytes.size())
        {
            break;
        }

        const uint32_t valueSize = ReadLE32(itemBytes.data() + cursor);
        const uint32_t itemFlags = ReadLE32(itemBytes.data() + cursor + 4);
        cursor += 8;

        // Scan for key NUL terminator
        const size_t keyStart = cursor;
        while (cursor < itemBytes.size() && itemBytes[cursor] != 0)
        {
            ++cursor;
        }
        if (cursor >= itemBytes.size())
        {
            break;
        }
        const size_t keyLen = cursor - keyStart;
        ++cursor; // skip NUL terminator

        // Convert to size_t once to prevent unsigned wraparound in bounds check.
        const std::size_t valueSizeSz = static_cast<std::size_t>(valueSize);
        if (valueSizeSz > itemBytes.size() - cursor)
        {
            break;
        }

        const uint8_t *valueData = itemBytes.data() + cursor;
        const std::string_view key(
            reinterpret_cast<const char *>(itemBytes.data() + keyStart), keyLen);

        // Encoding: bits 2-1 of itemFlags
        //   0 = UTF-8 text
        //   1 = Binary
        //   2 = External reference (skip)
        //   3 = Reserved (skip)
        const uint32_t encoding = (itemFlags >> 1) & 3;

        switch (encoding)
        {
        case 0: // UTF-8 text
            ProcessApeTextItem(metadata, key, valueData, valueSize);
            break;

        case 1: // Binary — only process cover art keys
            if (IEquals(key, "COVER ART (FRONT)") || IEquals(key, "COVER ART (BACK)"))
            {
                ProcessApeCoverItem(context, metadata, valueData, valueSize);
            }
            break;

        case 2: // External reference — skip
        case 3: // Reserved — skip
        default:
            break;
        }

        cursor += valueSizeSz;
    }
}

void ReadApeLyrics(ReadContext &context, RawLyrics &lyrics)
{
    if (context.fileSize < 32)
    {
        return;
    }

    uint32_t version = 0;
    uint32_t tagSize = 0;
    uint32_t itemCount = 0;
    uint32_t flags = 0;

    if (!FindApeFooter(context, version, tagSize, itemCount, flags))
    {
        return;
    }

    // APEv1 tags are unsupported
    if (version < 2000)
    {
        return;
    }

    if (tagSize > kMaxApeTagBytes || itemCount > kMaxApeItems)
    {
        return;
    }

    const bool hasHeader = (flags & 0x80000000) != 0;
    const uint64_t itemRegionOffset = hasHeader
        ? context.fileSize - static_cast<uint64_t>(tagSize)
        : context.fileSize - 32 - static_cast<uint64_t>(tagSize);

    const std::vector<uint8_t> itemBytes =
        ReadRange(context.input, itemRegionOffset, tagSize, kMaxApeTagBytes);
    if (itemBytes.size() < tagSize)
    {
        return;
    }

    size_t cursor = 0;
    for (uint32_t i = 0; i < itemCount; ++i)
    {
        if (cursor + 10 > itemBytes.size())
        {
            break;
        }

        const uint32_t valueSize = ReadLE32(itemBytes.data() + cursor);
        const uint32_t itemFlags = ReadLE32(itemBytes.data() + cursor + 4);
        cursor += 8;

        // Scan for key NUL terminator
        const size_t keyStart = cursor;
        while (cursor < itemBytes.size() && itemBytes[cursor] != 0)
        {
            ++cursor;
        }
        if (cursor >= itemBytes.size())
        {
            break;
        }
        const size_t keyLen = cursor - keyStart;
        ++cursor; // skip NUL terminator

        // Convert to size_t once to prevent unsigned wraparound in bounds check.
        const std::size_t valueSizeSz = static_cast<std::size_t>(valueSize);
        if (valueSizeSz > itemBytes.size() - cursor)
        {
            break;
        }

        const uint8_t *valueData = itemBytes.data() + cursor;
        const std::string_view key(
            reinterpret_cast<const char *>(itemBytes.data() + keyStart), keyLen);

        const uint32_t encoding = (itemFlags >> 1) & 3;

        // Only process UTF-8 text items for lyrics
        if (encoding == 0)
        {
            if (valueSize > kMaxApeItemValueBytes)
            {
                cursor += valueSizeSz;
                continue;
            }

            if (IEquals(key, "LYRICS") || IEquals(key, "UNSYNCED LYRICS") ||
                IEquals(key, "UNSYNCEDLYRICS"))
            {
                const std::string text = ReadUtf8Text(valueData, valueSize);
                ReadLyricsFromPlainText(lyrics, text);
            }
        }

        cursor += valueSizeSz;
    }
}

} // namespace tagreader_ape
