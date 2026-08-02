#include "formats/matroska/MatroskaParser.hpp"

#include "common/ParseHelpers.hpp"
#include "cover/CoverCache.hpp"
#include "formats/common/BoundedReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <cstdint>
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
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_cover::ExportCoverFromContext;
using tagreader_text::ReadUtf8Text;
using tagreader_text::TrimText;

namespace bounded = tagreader_core::formats;

constexpr std::uint64_t kEbmlId = 0x1A45DFA3;
constexpr std::uint64_t kSegmentId = 0x18538067;
constexpr std::uint64_t kTagsId = 0x1254C367;
constexpr std::uint64_t kTagId = 0x7373;
constexpr std::uint64_t kSimpleTagId = 0x67C8;
constexpr std::uint64_t kTagNameId = 0x45A3;
constexpr std::uint64_t kTagStringId = 0x4487;
constexpr std::uint64_t kAttachmentsId = 0x1941A469;
constexpr std::uint64_t kAttachedFileId = 0x61A7;
constexpr std::uint64_t kFileNameId = 0x466E;
constexpr std::uint64_t kFileMediaTypeId = 0x4660;
constexpr std::uint64_t kFileDataId = 0x465C;

constexpr std::uint64_t kMaxMatroskaScanBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxMatroskaElementPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxMatroskaTextBytes = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxMatroskaAttachmentImageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxMatroskaElements = 100000;
constexpr std::uint32_t kMaxMatroskaDepth = 16;

struct EbmlVint
{
    std::uint64_t value{};
    std::uint8_t length{};
    bool unknownSize{};
};

struct EbmlElement
{
    std::uint64_t id{};
    std::uint64_t headerOffset{};
    std::uint64_t payloadOffset{};
    std::uint64_t payloadSize{};
    std::uint64_t payloadEnd{};
    bool unknownSize{};
};

struct ParserState
{
    std::uint64_t elementsSeen{};
};

struct SimpleTagState
{
    std::string name;
    std::string value;
};

struct AttachedFileState
{
    std::string fileName;
    std::string mediaType;
    std::uint64_t dataOffset{};
    std::uint64_t dataSize{};
    bool hasData{};
};

std::optional<EbmlVint> ReadEbmlVint(ReadContext &context, std::uint64_t offset, std::uint64_t parentEnd, bool keepMarker)
{
    const std::vector<std::uint8_t> first = bounded::ReadRangeAt(context, offset, 1, parentEnd, 1);
    if (first.size() != 1 || first[0] == 0)
    {
        return std::nullopt;
    }

    std::uint8_t marker = 0x80;
    std::uint8_t length = 1;
    while ((first[0] & marker) == 0)
    {
        marker >>= 1;
        ++length;
        if (length > 8 || marker == 0)
        {
            return std::nullopt;
        }
    }

    const std::vector<std::uint8_t> bytes = bounded::ReadRangeAt(context, offset, length, parentEnd, length);
    if (bytes.size() != length)
    {
        return std::nullopt;
    }

    std::uint64_t value = keepMarker ? bytes[0] : static_cast<std::uint8_t>(bytes[0] & ~marker);
    std::uint64_t unknownValue = static_cast<std::uint8_t>(~marker);
    for (std::uint8_t i = 1; i < length; ++i)
    {
        value = (value << 8) | bytes[i];
        unknownValue = (unknownValue << 8) | 0xFFU;
    }

    return EbmlVint{value, length, !keepMarker && value == unknownValue};
}

std::optional<EbmlElement> ReadEbmlElement(ReadContext &context, std::uint64_t offset, std::uint64_t parentEnd)
{
    const std::optional<EbmlVint> id = ReadEbmlVint(context, offset, parentEnd, true);
    if (!id.has_value())
    {
        return std::nullopt;
    }
    const std::uint64_t sizeOffset = offset + id->length;
    if (sizeOffset < offset || sizeOffset >= parentEnd)
    {
        return std::nullopt;
    }
    const std::optional<EbmlVint> size = ReadEbmlVint(context, sizeOffset, parentEnd, false);
    if (!size.has_value())
    {
        return std::nullopt;
    }
    const std::uint64_t payloadOffset = sizeOffset + size->length;
    if (payloadOffset < sizeOffset || payloadOffset > parentEnd)
    {
        return std::nullopt;
    }

    const std::uint64_t payloadSize = size->unknownSize ? parentEnd - payloadOffset : size->value;
    const std::optional<bounded::BoundedRange> range = bounded::MakeBoundedRange(payloadOffset, payloadSize, parentEnd);
    if (!range.has_value())
    {
        return std::nullopt;
    }

    return EbmlElement{id->value, offset, payloadOffset, payloadSize, range->end, size->unknownSize};
}

bool IsMasterElement(std::uint64_t id)
{
    return id == kEbmlId || id == kSegmentId || id == kTagsId || id == kTagId || id == kSimpleTagId || id == kAttachmentsId || id == kAttachedFileId;
}

std::uint64_t ScanEndForElement(const EbmlElement &element, std::uint64_t rootScanEnd)
{
    return element.unknownSize ? rootScanEnd : element.payloadEnd;
}

std::string ReadUtf8Element(ReadContext &context, const EbmlElement &element)
{
    if (element.unknownSize || element.payloadSize == 0 || element.payloadSize > kMaxMatroskaTextBytes)
    {
        return {};
    }
    const std::vector<std::uint8_t> bytes = bounded::ReadRangeAt(context, element.payloadOffset, element.payloadSize, element.payloadEnd, kMaxMatroskaTextBytes);
    if (bytes.size() != element.payloadSize)
    {
        return {};
    }
    return TrimText(ReadUtf8Text(bytes.data(), bytes.size()));
}

void ApplySimpleTag(RawMetadata &metadata, const SimpleTagState &tag)
{
    if (tag.name.empty() || tag.value.empty())
    {
        return;
    }

    if ((IEquals(tag.name, "TITLE") || IEquals(tag.name, "Title")) && metadata.title.empty())
    {
        metadata.title = tag.value;
    }
    else if ((IEquals(tag.name, "ARTIST") || IEquals(tag.name, "LEAD_PERFORMER") || IEquals(tag.name, "PERFORMER")) && metadata.artist.empty())
    {
        metadata.artist = tag.value;
    }
    else if ((IEquals(tag.name, "ALBUM") || IEquals(tag.name, "PART_OF_A_SET")) && metadata.album.empty())
    {
        metadata.album = tag.value;
    }
    else if ((IEquals(tag.name, "ALBUMARTIST") || IEquals(tag.name, "ALBUM_ARTIST") || IEquals(tag.name, "ENSEMBLE")) && metadata.albumArtist.empty())
    {
        metadata.albumArtist = tag.value;
    }
    else if ((IEquals(tag.name, "COMPOSER") || IEquals(tag.name, "COMPOSED_BY")) && metadata.composer.empty())
    {
        metadata.composer = tag.value;
    }
    else if (IEquals(tag.name, "GENRE") && metadata.genre.empty())
    {
        metadata.genre = tag.value;
    }
    else if ((IEquals(tag.name, "DATE_RELEASED") || IEquals(tag.name, "DATE") || IEquals(tag.name, "YEAR")) && metadata.year == 0)
    {
        metadata.year = ParseYearOnly(tag.value);
    }
    else if ((IEquals(tag.name, "TRACKNUMBER") || IEquals(tag.name, "TRACK_NUMBER") || IEquals(tag.name, "PART_NUMBER")) && metadata.trackNumber == 0)
    {
        metadata.trackNumber = ParseSlashNumber(tag.value).first;
    }
    else if ((IEquals(tag.name, "DISCNUMBER") || IEquals(tag.name, "DISC_NUMBER")) && metadata.discNumber == 0)
    {
        metadata.discNumber = ParseSlashNumber(tag.value).first;
    }
    else if ((IEquals(tag.name, "COMMENT") || IEquals(tag.name, "DESCRIPTION")) && metadata.comment.empty())
    {
        metadata.comment = tag.value;
    }
}

bool IsImageMediaType(std::string_view mediaType)
{
    return mediaType.size() > 6 &&
           (mediaType[0] == 'i' || mediaType[0] == 'I') &&
           (mediaType[1] == 'm' || mediaType[1] == 'M') &&
           (mediaType[2] == 'a' || mediaType[2] == 'A') &&
           (mediaType[3] == 'g' || mediaType[3] == 'G') &&
           (mediaType[4] == 'e' || mediaType[4] == 'E') &&
           mediaType[5] == '/';
}

void ExportAttachedImage(ReadContext &context, RawMetadata &metadata, const AttachedFileState &attachedFile)
{
    if (!metadata.coverPath.empty() || !attachedFile.hasData || !IsImageMediaType(attachedFile.mediaType) ||
        attachedFile.dataSize == 0 || attachedFile.dataSize > kMaxMatroskaAttachmentImageBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> bytes = bounded::ReadRangeAt(context, attachedFile.dataOffset, attachedFile.dataSize,
                                                                 attachedFile.dataOffset + attachedFile.dataSize,
                                                                 kMaxMatroskaAttachmentImageBytes);
    if (bytes.size() != attachedFile.dataSize)
    {
        return;
    }

    const tagreader_cover::CoverPaths paths = ExportCoverFromContext(context, bytes.data(), bytes.size());
    if (!paths.fullSizePath.empty() || !paths.thumbnailPath.empty())
    {
        metadata.coverPath = paths.fullSizePath;
        metadata.thumbnailPath = paths.thumbnailPath;
    }
}

void ParseSimpleTag(ReadContext &context, const EbmlElement &simpleTag, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth);
void ParseTags(ReadContext &context, const EbmlElement &tags, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth);
void ParseAttachments(ReadContext &context, const EbmlElement &attachments, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth);

bool CountElement(ParserState &state)
{
    ++state.elementsSeen;
    return state.elementsSeen <= kMaxMatroskaElements;
}

void ParseSimpleTagChildren(ReadContext &context, const EbmlElement &simpleTag, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, SimpleTagState &tag, std::uint32_t depth)
{
    if (depth > kMaxMatroskaDepth)
    {
        return;
    }

    std::uint64_t cursor = simpleTag.payloadOffset;
    const std::uint64_t end = ScanEndForElement(simpleTag, rootScanEnd);
    while (cursor < end)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> child = ReadEbmlElement(context, cursor, end);
        if (!child.has_value() || child->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (child->id == kTagNameId)
        {
            tag.name = ReadUtf8Element(context, *child);
        }
        else if (child->id == kTagStringId)
        {
            tag.value = ReadUtf8Element(context, *child);
        }
        else if (child->id == kSimpleTagId)
        {
            ParseSimpleTag(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        cursor = child->payloadEnd;
    }
}

void ParseSimpleTag(ReadContext &context, const EbmlElement &simpleTag, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth)
{
    SimpleTagState tag{};
    ParseSimpleTagChildren(context, simpleTag, rootScanEnd, state, metadata, tag, depth);
    ApplySimpleTag(metadata, tag);
}

void ParseTags(ReadContext &context, const EbmlElement &tags, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth)
{
    if (depth > kMaxMatroskaDepth)
    {
        return;
    }

    std::uint64_t cursor = tags.payloadOffset;
    const std::uint64_t end = ScanEndForElement(tags, rootScanEnd);
    while (cursor < end)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> child = ReadEbmlElement(context, cursor, end);
        if (!child.has_value() || child->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (child->id == kTagId || child->id == kSimpleTagId)
        {
            ParseSimpleTag(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        cursor = child->payloadEnd;
    }
}

void ParseAttachedFile(ReadContext &context, const EbmlElement &attachedFile, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth)
{
    if (depth > kMaxMatroskaDepth)
    {
        return;
    }

    AttachedFileState file{};
    std::uint64_t cursor = attachedFile.payloadOffset;
    const std::uint64_t end = ScanEndForElement(attachedFile, rootScanEnd);
    while (cursor < end)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> child = ReadEbmlElement(context, cursor, end);
        if (!child.has_value() || child->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (child->id == kFileNameId)
        {
            file.fileName = ReadUtf8Element(context, *child);
        }
        else if (child->id == kFileMediaTypeId)
        {
            file.mediaType = ReadUtf8Element(context, *child);
        }
        else if (child->id == kFileDataId && !child->unknownSize)
        {
            file.dataOffset = child->payloadOffset;
            file.dataSize = child->payloadSize;
            file.hasData = true;
        }
        cursor = child->payloadEnd;
    }

    ExportAttachedImage(context, metadata, file);
}

void ParseAttachments(ReadContext &context, const EbmlElement &attachments, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth)
{
    if (depth > kMaxMatroskaDepth)
    {
        return;
    }

    std::uint64_t cursor = attachments.payloadOffset;
    const std::uint64_t end = ScanEndForElement(attachments, rootScanEnd);
    while (cursor < end)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> child = ReadEbmlElement(context, cursor, end);
        if (!child.has_value() || child->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (child->id == kAttachedFileId)
        {
            ParseAttachedFile(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        cursor = child->payloadEnd;
    }
}

void WalkElementChildren(ReadContext &context, const EbmlElement &parent, std::uint64_t rootScanEnd, ParserState &state, RawMetadata &metadata, std::uint32_t depth)
{
    if (depth > kMaxMatroskaDepth)
    {
        return;
    }

    std::uint64_t cursor = parent.payloadOffset;
    const std::uint64_t end = ScanEndForElement(parent, rootScanEnd);
    while (cursor < end)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> child = ReadEbmlElement(context, cursor, end);
        if (!child.has_value() || child->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (child->id == kTagsId)
        {
            ParseTags(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        else if (child->id == kAttachmentsId)
        {
            ParseAttachments(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        else if (IsMasterElement(child->id))
        {
            WalkElementChildren(context, *child, rootScanEnd, state, metadata, depth + 1);
        }
        cursor = child->payloadEnd;
    }
}

void ParseMatroska(ReadContext &context, RawMetadata &metadata)
{
    const std::uint64_t fileSize = static_cast<std::uint64_t>(context.fileSize);
    const std::uint64_t rootScanEnd = std::min<std::uint64_t>(fileSize, kMaxMatroskaScanBytes);
    ParserState state{};

    std::uint64_t cursor = 0;
    while (cursor < rootScanEnd)
    {
        if (!CountElement(state))
        {
            return;
        }
        const std::optional<EbmlElement> element = ReadEbmlElement(context, cursor, rootScanEnd);
        if (!element.has_value() || element->payloadSize > kMaxMatroskaElementPayloadBytes)
        {
            return;
        }
        if (element->id == kSegmentId || element->id == kEbmlId)
        {
            WalkElementChildren(context, *element, rootScanEnd, state, metadata, 1);
        }
        cursor = element->payloadEnd;
    }
}
}

namespace tagreader_matroska
{
void ReadMatroskaMetadata(ReadContext &context, RawMetadata &metadata)
{
    ParseMatroska(context, metadata);
}
}
