#include "formats/aiff/AiffParser.hpp"

#include "formats/common/BoundedReader.hpp"
#include "formats/id3/Id3Parser.hpp"
#include "text/TextCodec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
namespace bounded = tagreader_core::formats;

constexpr std::uint64_t kFormHeaderBytes = 12;
constexpr std::uint64_t kChunkHeaderBytes = 8;
constexpr std::uint64_t kMaxNativeChunkBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxId3ChunkBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxAiffChunks = 100000;

bool MatchesFourCc(std::span<const std::uint8_t> bytes, std::string_view fourCc)
{
    return bytes.size() == 4 && fourCc.size() == 4 &&
           std::memcmp(bytes.data(), fourCc.data(), 4) == 0;
}

bool ValidateAiffMagic(ReadContext &context, std::uint64_t &formEnd)
{
    const std::vector<std::uint8_t> header = bounded::ReadRangeAt(context, 0, kFormHeaderBytes, context.fileSize, kFormHeaderBytes);
    if (header.size() != kFormHeaderBytes || std::memcmp(header.data(), "FORM", 4) != 0 ||
        (std::memcmp(header.data() + 8, "AIFF", 4) != 0 && std::memcmp(header.data() + 8, "AIFC", 4) != 0))
    {
        return false;
    }

    const std::optional<std::uint32_t> formPayloadSize = bounded::ReadU32Be(std::span<const std::uint8_t>(header).subspan(4, 4));
    if (!formPayloadSize.has_value())
    {
        return false;
    }

    const std::optional<bounded::BoundedRange> formRange = bounded::MakeBoundedRange(8, *formPayloadSize, context.fileSize);
    if (!formRange.has_value())
    {
        return false;
    }

    formEnd = formRange->end;
    return formEnd >= kFormHeaderBytes;
}

std::string DecodeNativeText(std::span<const std::uint8_t> bytes)
{
    std::size_t size = bytes.size();
    while (size > 0 && bytes[size - 1] == 0)
    {
        --size;
    }
    if (size == 0)
    {
        return {};
    }

    const std::string_view raw(reinterpret_cast<const char *>(bytes.data()), size);
    const tagreader_core::DecodedField field = tagreader_text::DecodeRawText(raw);
    return field.success ? field.value : std::string{};
}

void ApplyNativeTextField(RawMetadata &metadata, std::string_view chunkId, const std::string &value)
{
    if (value.empty())
    {
        return;
    }

    if (chunkId == "NAME" && metadata.title.empty())
    {
        metadata.title = value;
    }
    else if (chunkId == "AUTH" && metadata.artist.empty())
    {
        metadata.artist = value;
    }
    else if ((chunkId == "ANNO" || chunkId == "(c) ") && metadata.comment.empty())
    {
        metadata.comment = value;
    }
}

void ReadNativeTextChunk(ReadContext &context, const bounded::BoundedChunkRange &chunk, std::string_view chunkId, RawMetadata &nativeMetadata)
{
    if (chunk.payloadSize > kMaxNativeChunkBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> valueBytes = bounded::ReadRangeAt(context, chunk.payloadOffset, chunk.payloadSize, chunk.payloadEnd, kMaxNativeChunkBytes);
    if (valueBytes.size() != chunk.payloadSize)
    {
        return;
    }

    ApplyNativeTextField(nativeMetadata, chunkId, DecodeNativeText(valueBytes));
}

void ReadComtChunk(ReadContext &context, const bounded::BoundedChunkRange &chunk, RawMetadata &nativeMetadata)
{
    if (!nativeMetadata.comment.empty() || chunk.payloadSize > kMaxNativeChunkBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> bytes = bounded::ReadRangeAt(context, chunk.payloadOffset, chunk.payloadSize, chunk.payloadEnd, kMaxNativeChunkBytes);
    if (bytes.size() != chunk.payloadSize || bytes.size() < 2)
    {
        return;
    }

    bounded::BoundedCursor cursor(bytes);
    const std::optional<std::uint16_t> commentCount = cursor.readU16Be();
    if (!commentCount.has_value())
    {
        return;
    }

    for (std::uint16_t index = 0; index < *commentCount; ++index)
    {
        if (!cursor.skip(4) || !cursor.skip(2))
        {
            return;
        }
        const std::optional<std::uint16_t> markerLength = cursor.readU16Be();
        if (!markerLength.has_value())
        {
            return;
        }
        const std::optional<std::span<const std::uint8_t>> markerText = cursor.readBytes(*markerLength);
        if (!markerText.has_value())
        {
            return;
        }
        if ((*markerLength % 2) != 0 && !cursor.skip(1))
        {
            return;
        }

        const std::string decoded = DecodeNativeText(*markerText);
        if (!decoded.empty())
        {
            nativeMetadata.comment = decoded;
            return;
        }
    }
}

void FillMissingMetadata(RawMetadata &target, const RawMetadata &fallback)
{
    if (target.title.empty())
        target.title = fallback.title;
    if (target.genre.empty())
        target.genre = fallback.genre;
    if (target.artist.empty())
        target.artist = fallback.artist;
    if (target.album.empty())
        target.album = fallback.album;
    if (target.albumArtist.empty())
        target.albumArtist = fallback.albumArtist;
    if (target.composer.empty())
        target.composer = fallback.composer;
    if (target.comment.empty())
        target.comment = fallback.comment;
    if (target.year == 0)
        target.year = fallback.year;
    if (target.trackNumber == 0)
        target.trackNumber = fallback.trackNumber;
    if (target.discNumber == 0)
        target.discNumber = fallback.discNumber;
    if (target.coverPath.empty())
        target.coverPath = fallback.coverPath;
}

void ReadEmbeddedId3(ReadContext &context, const bounded::BoundedChunkRange &chunk, RawMetadata &id3Metadata)
{
    if (chunk.payloadSize > kMaxId3ChunkBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> id3Bytes = bounded::ReadRangeAt(context, chunk.payloadOffset, chunk.payloadSize, chunk.payloadEnd, kMaxId3ChunkBytes);
    if (id3Bytes.size() != chunk.payloadSize)
    {
        return;
    }

    tagreader_id3::ReadID3v2MetadataFromBytes(context, id3Bytes, id3Metadata);
}
}

namespace tagreader_aiff
{
void ReadAiffMetadata(ReadContext &context, RawMetadata &metadata)
{
    std::uint64_t formEnd = 0;
    if (!ValidateAiffMagic(context, formEnd))
    {
        return;
    }

    RawMetadata nativeMetadata{};
    RawMetadata id3Metadata{};

    std::uint64_t cursor = kFormHeaderBytes;
    std::uint64_t chunkCount = 0;
    while (cursor + kChunkHeaderBytes <= formEnd && chunkCount < kMaxAiffChunks)
    {
        ++chunkCount;
        const std::vector<std::uint8_t> chunkHeader = bounded::ReadRangeAt(context, cursor, kChunkHeaderBytes, formEnd, kChunkHeaderBytes);
        if (chunkHeader.size() != kChunkHeaderBytes)
        {
            break;
        }

        const std::optional<std::uint32_t> chunkSize = bounded::ReadU32Be(std::span<const std::uint8_t>(chunkHeader).subspan(4, 4));
        if (!chunkSize.has_value())
        {
            break;
        }

        const std::optional<bounded::BoundedChunkRange> chunk = bounded::MakeBoundedChunkRange(cursor + kChunkHeaderBytes, *chunkSize, formEnd);
        if (!chunk.has_value())
        {
            break;
        }

        const std::span<const std::uint8_t> chunkId(chunkHeader.data(), 4);
        const std::string_view chunkIdView(reinterpret_cast<const char *>(chunkHeader.data()), 4);
        if (MatchesFourCc(chunkId, "NAME") || MatchesFourCc(chunkId, "AUTH") || MatchesFourCc(chunkId, "ANNO") || MatchesFourCc(chunkId, "(c) "))
        {
            ReadNativeTextChunk(context, *chunk, chunkIdView, nativeMetadata);
        }
        else if (MatchesFourCc(chunkId, "COMT"))
        {
            ReadComtChunk(context, *chunk, nativeMetadata);
        }
        else if (MatchesFourCc(chunkId, "ID3 "))
        {
            ReadEmbeddedId3(context, *chunk, id3Metadata);
        }

        cursor = chunk->paddedEnd;
    }

    metadata = std::move(id3Metadata);
    FillMissingMetadata(metadata, nativeMetadata);
}
}
