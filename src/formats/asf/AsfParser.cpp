#include "formats/asf/AsfParser.hpp"

#include "common/ParseHelpers.hpp"
#include "cover/CoverCache.hpp"
#include "formats/common/BoundedReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using tagreader_common::IEquals;
using tagreader_common::ParseSlashNumber;
using tagreader_common::ParseYearOnly;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_cover::ExportCoverFromContext;
using tagreader_text::ReadLyricsFromPlainText;
using tagreader_text::ReadUtf16Text;

namespace bounded = tagreader_core::formats;

using Guid = std::array<std::uint8_t, 16>;

constexpr Guid kAsfHeaderObjectGuid{0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr Guid kAsfContentDescriptionObjectGuid{0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                                0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr Guid kAsfExtendedContentDescriptionObjectGuid{0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11,
                                                        0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
constexpr Guid kAsfMetadataObjectGuid{0xEA, 0xCB, 0xF8, 0xC5, 0xAF, 0x5B, 0x77, 0x48,
                                      0x84, 0x67, 0xAA, 0x8C, 0x44, 0xFA, 0x4C, 0xCA};
constexpr Guid kAsfMetadataLibraryObjectGuid{0x94, 0x1C, 0x23, 0x44, 0x98, 0x94, 0xD1, 0x49,
                                             0xA1, 0x41, 0x1D, 0x13, 0x4E, 0x45, 0x70, 0x54};

constexpr std::uint64_t kAsfObjectHeaderBytes = 24;
constexpr std::uint64_t kAsfHeaderObjectExtraBytes = 6;
constexpr std::uint64_t kMaxAsfHeaderBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxAsfMetadataObjectBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxAsfObjects = 100000;
constexpr std::uint32_t kMaxAsfDescriptors = 4096;
constexpr std::uint32_t kMaxAsfTextDescriptorBytes = 1U * 1024U * 1024U;
constexpr std::uint32_t kMaxAsfImageDescriptorBytes = 64U * 1024U * 1024U;

struct AsfObjectHeader
{
    Guid guid{};
    std::uint64_t size{};
};

struct ParsedHeaderObject
{
    std::uint64_t end{};
    std::uint32_t childCount{};
};

std::optional<AsfObjectHeader> ReadAsfObjectHeader(ReadContext &context, std::uint64_t offset, std::uint64_t parentEnd)
{
    const std::vector<std::uint8_t> bytes = bounded::ReadRangeAt(context, offset, kAsfObjectHeaderBytes, parentEnd, kAsfObjectHeaderBytes);
    if (bytes.size() != kAsfObjectHeaderBytes)
    {
        return std::nullopt;
    }

    AsfObjectHeader header{};
    std::copy_n(bytes.begin(), header.guid.size(), header.guid.begin());
    const std::optional<std::uint64_t> size = bounded::ReadU64Le(std::span<const std::uint8_t>(bytes).subspan(16, 8));
    if (!size.has_value() || *size < kAsfObjectHeaderBytes)
    {
        return std::nullopt;
    }
    header.size = *size;
    return header;
}

std::optional<ParsedHeaderObject> ValidateHeaderObject(ReadContext &context)
{
    const std::optional<AsfObjectHeader> header = ReadAsfObjectHeader(context, 0, context.fileSize);
    if (!header.has_value() || header->guid != kAsfHeaderObjectGuid || header->size > kMaxAsfHeaderBytes)
    {
        return std::nullopt;
    }
    const std::optional<bounded::BoundedRange> range = bounded::MakeBoundedRange(0, header->size, context.fileSize);
    if (!range.has_value() || range->size < kAsfObjectHeaderBytes + kAsfHeaderObjectExtraBytes)
    {
        return std::nullopt;
    }

    const std::vector<std::uint8_t> extra = bounded::ReadRangeAt(context, kAsfObjectHeaderBytes, kAsfHeaderObjectExtraBytes, range->end, kAsfHeaderObjectExtraBytes);
    if (extra.size() != kAsfHeaderObjectExtraBytes || extra[4] != 0x01 || extra[5] != 0x02)
    {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> childCount = bounded::ReadU32Le(std::span<const std::uint8_t>(extra).subspan(0, 4));
    if (!childCount.has_value())
    {
        return std::nullopt;
    }
    return ParsedHeaderObject{range->end, *childCount};
}

std::string DecodeUtf16Le(std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() % 2) != 0 || bytes.size() > kMaxAsfTextDescriptorBytes)
    {
        return {};
    }
    return ReadUtf16Text(bytes.data(), bytes.size(), false);
}

void ApplyTextDescriptor(RawMetadata *metadata, RawLyrics *lyrics, std::string_view name, const std::string &value)
{
    if (value.empty())
    {
        return;
    }

    if (metadata != nullptr)
    {
        if ((IEquals(name, "Title") || IEquals(name, "WM/Title")) && metadata->title.empty())
        {
            metadata->title = value;
        }
        else if ((IEquals(name, "Author") || IEquals(name, "Artist") || IEquals(name, "WM/Author") || IEquals(name, "WM/Artist")) && metadata->artist.empty())
        {
            metadata->artist = value;
        }
        else if ((IEquals(name, "WM/AlbumTitle") || IEquals(name, "AlbumTitle") || IEquals(name, "Album")) && metadata->album.empty())
        {
            metadata->album = value;
        }
        else if ((IEquals(name, "WM/AlbumArtist") || IEquals(name, "AlbumArtist") || IEquals(name, "Album Artist")) && metadata->albumArtist.empty())
        {
            metadata->albumArtist = value;
        }
        else if ((IEquals(name, "WM/Composer") || IEquals(name, "Composer")) && metadata->composer.empty())
        {
            metadata->composer = value;
        }
        else if ((IEquals(name, "WM/Genre") || IEquals(name, "Genre")) && metadata->genre.empty())
        {
            metadata->genre = value;
        }
        else if (IEquals(name, "WM/Year") || IEquals(name, "WM/OriginalReleaseYear") || IEquals(name, "Year"))
        {
            metadata->year = metadata->year == 0 ? ParseYearOnly(value) : metadata->year;
        }
        else if (IEquals(name, "WM/TrackNumber") || IEquals(name, "WM/Track") || IEquals(name, "TrackNumber") || IEquals(name, "Track"))
        {
            metadata->trackNumber = metadata->trackNumber == 0 ? ParseSlashNumber(value).first : metadata->trackNumber;
        }
        else if ((IEquals(name, "Description") || IEquals(name, "WM/Description") || IEquals(name, "Comment")) && metadata->comment.empty())
        {
            metadata->comment = value;
        }
    }

    if (lyrics != nullptr && (IEquals(name, "WM/Lyrics") || IEquals(name, "Lyrics") || IEquals(name, "UNSYNCED LYRICS") || IEquals(name, "UNSYNCEDLYRICS")))
    {
        ReadLyricsFromPlainText(*lyrics, value);
    }
}

void ApplyNumberDescriptor(RawMetadata *metadata, std::string_view name, std::uint64_t value)
{
    if (metadata == nullptr || value == 0 || value > std::numeric_limits<std::uint16_t>::max())
    {
        return;
    }
    if ((IEquals(name, "WM/TrackNumber") || IEquals(name, "WM/Track") || IEquals(name, "TrackNumber") || IEquals(name, "Track")) && metadata->trackNumber == 0)
    {
        metadata->trackNumber = static_cast<std::uint16_t>(value);
    }
    else if ((IEquals(name, "WM/Year") || IEquals(name, "Year")) && metadata->year == 0)
    {
        metadata->year = static_cast<std::uint16_t>(value);
    }
}

std::optional<std::uint64_t> ReadDescriptorNumber(std::uint16_t valueType, std::span<const std::uint8_t> valueBytes)
{
    if (valueType == 5 && valueBytes.size() >= 2)
    {
        return bounded::ReadU16Le(valueBytes.subspan(0, 2));
    }
    if ((valueType == 2 || valueType == 3) && valueBytes.size() >= 4)
    {
        return bounded::ReadU32Le(valueBytes.subspan(0, 4));
    }
    if (valueType == 4 && valueBytes.size() >= 8)
    {
        return bounded::ReadU64Le(valueBytes.subspan(0, 8));
    }
    return std::nullopt;
}

std::optional<std::size_t> FindUtf16Terminator(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    for (std::size_t cursor = offset; cursor + 1 < bytes.size(); cursor += 2)
    {
        if (bytes[cursor] == 0 && bytes[cursor + 1] == 0)
        {
            return cursor;
        }
    }
    return std::nullopt;
}

void ProcessPictureDescriptor(ReadContext &context, RawMetadata *metadata, std::span<const std::uint8_t> bytes)
{
    if (metadata == nullptr || !metadata->coverPath.empty() || bytes.size() < 5 || bytes.size() > kMaxAsfImageDescriptorBytes)
    {
        return;
    }

    const std::optional<std::uint32_t> imageSize = bounded::ReadU32Le(bytes.subspan(1, 4));
    if (!imageSize.has_value() || *imageSize == 0 || *imageSize > kMaxAsfImageDescriptorBytes)
    {
        return;
    }

    const std::optional<std::size_t> mimeEnd = FindUtf16Terminator(bytes, 5);
    if (!mimeEnd.has_value())
    {
        return;
    }
    const std::size_t descriptionOffset = *mimeEnd + 2;
    const std::optional<std::size_t> descriptionEnd = FindUtf16Terminator(bytes, descriptionOffset);
    if (!descriptionEnd.has_value())
    {
        return;
    }
    const std::size_t imageOffset = *descriptionEnd + 2;
    if (imageOffset > bytes.size() || *imageSize > bytes.size() - imageOffset)
    {
        return;
    }

    const tagreader_cover::CoverPaths paths = ExportCoverFromContext(context, bytes.data() + imageOffset, *imageSize);
    if (!paths.fullSizePath.empty() || !paths.thumbnailPath.empty())
    {
        metadata->coverPath = paths.fullSizePath;
        metadata->thumbnailPath = paths.thumbnailPath;
    }
}

void ProcessDescriptor(ReadContext &context, RawMetadata *metadata, RawLyrics *lyrics, std::string_view name, std::uint16_t valueType, std::span<const std::uint8_t> valueBytes)
{
    if (valueType == 0)
    {
        ApplyTextDescriptor(metadata, lyrics, name, DecodeUtf16Le(valueBytes));
        return;
    }
    if (valueType == 1)
    {
        if (IEquals(name, "WM/Picture") || IEquals(name, "Picture"))
        {
            ProcessPictureDescriptor(context, metadata, valueBytes);
        }
        return;
    }
    const std::optional<std::uint64_t> number = ReadDescriptorNumber(valueType, valueBytes);
    if (number.has_value())
    {
        ApplyNumberDescriptor(metadata, name, *number);
    }
}

void ReadContentDescriptionObject(ReadContext &, std::span<const std::uint8_t> payload, RawMetadata *metadata, RawLyrics *lyrics)
{
    if (payload.size() < 10)
    {
        return;
    }
    std::array<std::uint16_t, 5> lengths{};
    for (std::size_t i = 0; i < lengths.size(); ++i)
    {
        const std::optional<std::uint16_t> length = bounded::ReadU16Le(payload.subspan(i * 2, 2));
        if (!length.has_value())
        {
            return;
        }
        lengths[i] = *length;
    }

    std::size_t cursor = 10;
    constexpr std::array<std::string_view, 5> names{"Title", "Author", "Copyright", "Description", "Rating"};
    for (std::size_t i = 0; i < lengths.size(); ++i)
    {
        if (lengths[i] > payload.size() - cursor)
        {
            return;
        }
        const std::span<const std::uint8_t> textBytes = payload.subspan(cursor, lengths[i]);
        if (names[i] == "Copyright")
        {
            if (metadata != nullptr && metadata->comment.empty())
            {
                metadata->comment = DecodeUtf16Le(textBytes);
            }
        }
        else
        {
            ApplyTextDescriptor(metadata, lyrics, names[i], DecodeUtf16Le(textBytes));
        }
        cursor += lengths[i];
    }
}

void ReadExtendedContentDescriptionObject(ReadContext &context, std::span<const std::uint8_t> payload, RawMetadata *metadata, RawLyrics *lyrics)
{
    if (payload.size() < 2)
    {
        return;
    }
    const std::optional<std::uint16_t> descriptorCount = bounded::ReadU16Le(payload.subspan(0, 2));
    if (!descriptorCount.has_value() || *descriptorCount > kMaxAsfDescriptors)
    {
        return;
    }

    std::size_t cursor = 2;
    for (std::uint16_t i = 0; i < *descriptorCount; ++i)
    {
        if (cursor + 6 > payload.size())
        {
            break;
        }
        const std::optional<std::uint16_t> nameLength = bounded::ReadU16Le(payload.subspan(cursor, 2));
        cursor += 2;
        if (!nameLength.has_value() || *nameLength == 0 || *nameLength > kMaxAsfTextDescriptorBytes || *nameLength > payload.size() - cursor)
        {
            break;
        }
        const std::string name = DecodeUtf16Le(payload.subspan(cursor, *nameLength));
        cursor += *nameLength;
        if (cursor + 4 > payload.size())
        {
            break;
        }
        const std::optional<std::uint16_t> valueType = bounded::ReadU16Le(payload.subspan(cursor, 2));
        const std::optional<std::uint16_t> valueLength = bounded::ReadU16Le(payload.subspan(cursor + 2, 2));
        cursor += 4;
        if (!valueType.has_value() || !valueLength.has_value() || *valueLength > payload.size() - cursor)
        {
            break;
        }
        const bool oversizedText = *valueType == 0 && *valueLength > kMaxAsfTextDescriptorBytes;
        const bool oversizedImage = *valueType == 1 && *valueLength > kMaxAsfImageDescriptorBytes;
        if (!name.empty() && !oversizedText && !oversizedImage)
        {
            ProcessDescriptor(context, metadata, lyrics, name, *valueType, payload.subspan(cursor, *valueLength));
        }
        cursor += *valueLength;
    }
}

void ReadMetadataObject(ReadContext &context, std::span<const std::uint8_t> payload, RawMetadata *metadata, RawLyrics *lyrics)
{
    if (payload.size() < 2)
    {
        return;
    }
    const std::optional<std::uint16_t> descriptorCount = bounded::ReadU16Le(payload.subspan(0, 2));
    if (!descriptorCount.has_value() || *descriptorCount > kMaxAsfDescriptors)
    {
        return;
    }

    std::size_t cursor = 2;
    for (std::uint16_t i = 0; i < *descriptorCount; ++i)
    {
        if (cursor + 12 > payload.size())
        {
            break;
        }
        cursor += 4;
        const std::optional<std::uint16_t> nameLength = bounded::ReadU16Le(payload.subspan(cursor, 2));
        const std::optional<std::uint16_t> valueType = bounded::ReadU16Le(payload.subspan(cursor + 2, 2));
        const std::optional<std::uint32_t> valueLength = bounded::ReadU32Le(payload.subspan(cursor + 4, 4));
        cursor += 8;
        if (!nameLength.has_value() || !valueType.has_value() || !valueLength.has_value() || *nameLength == 0 || *nameLength > kMaxAsfTextDescriptorBytes ||
            *nameLength > payload.size() - cursor)
        {
            break;
        }
        const std::string name = DecodeUtf16Le(payload.subspan(cursor, *nameLength));
        cursor += *nameLength;
        if (*valueLength > payload.size() - cursor)
        {
            break;
        }
        const bool oversizedText = *valueType == 0 && *valueLength > kMaxAsfTextDescriptorBytes;
        const bool oversizedImage = *valueType == 1 && *valueLength > kMaxAsfImageDescriptorBytes;
        if (!name.empty() && !oversizedText && !oversizedImage)
        {
            ProcessDescriptor(context, metadata, lyrics, name, *valueType, payload.subspan(cursor, *valueLength));
        }
        cursor += *valueLength;
    }
}

void ParseAsfHeader(ReadContext &context, RawMetadata *metadata, RawLyrics *lyrics)
{
    const std::optional<ParsedHeaderObject> header = ValidateHeaderObject(context);
    if (!header.has_value())
    {
        return;
    }

    std::uint64_t cursor = kAsfObjectHeaderBytes + kAsfHeaderObjectExtraBytes;
    const std::uint64_t objectLimit = std::min<std::uint64_t>(header->childCount, kMaxAsfObjects);
    for (std::uint64_t objectIndex = 0; objectIndex < objectLimit && cursor + kAsfObjectHeaderBytes <= header->end; ++objectIndex)
    {
        const std::optional<AsfObjectHeader> objectHeader = ReadAsfObjectHeader(context, cursor, header->end);
        if (!objectHeader.has_value())
        {
            break;
        }
        const std::optional<bounded::BoundedRange> objectRange = bounded::MakeBoundedRange(cursor, objectHeader->size, header->end);
        if (!objectRange.has_value())
        {
            break;
        }

        const std::uint64_t payloadOffset = cursor + kAsfObjectHeaderBytes;
        const std::uint64_t payloadSize = objectHeader->size - kAsfObjectHeaderBytes;
        const bool metadataObject = objectHeader->guid == kAsfContentDescriptionObjectGuid ||
                                    objectHeader->guid == kAsfExtendedContentDescriptionObjectGuid ||
                                    objectHeader->guid == kAsfMetadataObjectGuid ||
                                    objectHeader->guid == kAsfMetadataLibraryObjectGuid;
        if (metadataObject && payloadSize <= kMaxAsfMetadataObjectBytes)
        {
            const std::vector<std::uint8_t> payload = bounded::ReadRangeAt(context, payloadOffset, payloadSize, objectRange->end, kMaxAsfMetadataObjectBytes);
            if (payload.size() == payloadSize)
            {
                const std::span<const std::uint8_t> payloadSpan(payload.data(), payload.size());
                if (objectHeader->guid == kAsfContentDescriptionObjectGuid)
                {
                    ReadContentDescriptionObject(context, payloadSpan, metadata, lyrics);
                }
                else if (objectHeader->guid == kAsfExtendedContentDescriptionObjectGuid)
                {
                    ReadExtendedContentDescriptionObject(context, payloadSpan, metadata, lyrics);
                }
                else
                {
                    ReadMetadataObject(context, payloadSpan, metadata, lyrics);
                }
            }
        }

        cursor = objectRange->end;
        if (metadata != nullptr && lyrics == nullptr && !metadata->coverPath.empty() && !metadata->title.empty() && !metadata->artist.empty() && !metadata->album.empty() && !metadata->albumArtist.empty() && metadata->year != 0 && metadata->trackNumber != 0)
        {
            continue;
        }
    }
}
}

namespace tagreader_asf
{
void ReadAsfMetadata(ReadContext &context, RawMetadata &metadata)
{
    ParseAsfHeader(context, &metadata, nullptr);
}

void ReadAsfLyrics(ReadContext &context, RawLyrics &lyrics)
{
    ParseAsfHeader(context, nullptr, &lyrics);
}
}
