#include "formats/riff/RiffParser.hpp"

#include "common/ParseHelpers.hpp"
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
#include <utility>
#include <vector>

namespace
{
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
namespace bounded = tagreader_core::formats;

constexpr std::uint64_t kRiffHeaderBytes = 12;
constexpr std::uint64_t kChunkHeaderBytes = 8;
constexpr std::uint64_t kMaxListInfoBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxId3ChunkBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxRiffChunks = 100000;

bool MatchesFourCc(std::span<const std::uint8_t> bytes, std::string_view fourCc)
{
    return bytes.size() == 4 && fourCc.size() == 4 &&
           std::memcmp(bytes.data(), fourCc.data(), 4) == 0;
}

bool ValidateRiffWaveMagic(ReadContext &context, std::uint64_t &riffEnd)
{
    const std::vector<std::uint8_t> header = bounded::ReadRangeAt(context, 0, kRiffHeaderBytes, context.fileSize, kRiffHeaderBytes);
    if (header.size() != kRiffHeaderBytes ||
        std::memcmp(header.data(), "RIFF", 4) != 0 ||
        std::memcmp(header.data() + 8, "WAVE", 4) != 0)
    {
        return false;
    }

    const std::optional<std::uint32_t> riffPayloadSize = bounded::ReadU32Le(std::span<const std::uint8_t>(header).subspan(4, 4));
    if (!riffPayloadSize.has_value())
    {
        return false;
    }

    const std::optional<bounded::BoundedRange> riffRange = bounded::MakeBoundedRange(8, *riffPayloadSize, context.fileSize);
    if (!riffRange.has_value())
    {
        return false;
    }

    riffEnd = riffRange->end;
    return riffEnd >= kRiffHeaderBytes;
}

std::string DecodeInfoText(std::span<const std::uint8_t> bytes)
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

void ApplyInfoField(RawMetadata &metadata, std::string_view fieldId, const std::string &value)
{
    if (value.empty())
    {
        return;
    }

    if ((fieldId == "INAM" || fieldId == "TITL") && metadata.title.empty())
    {
        metadata.title = value;
    }
    else if ((fieldId == "IART" || fieldId == "ARTI") && metadata.artist.empty())
    {
        metadata.artist = value;
    }
    else if ((fieldId == "IPRD" || fieldId == "IPRO" || fieldId == "IALB") && metadata.album.empty())
    {
        metadata.album = value;
    }
    else if ((fieldId == "IGNR" || fieldId == "GENR") && metadata.genre.empty())
    {
        metadata.genre = value;
    }
    else if ((fieldId == "ICRD" || fieldId == "YEAR") && metadata.year == 0)
    {
        metadata.year = tagreader_common::ParseYearOnly(value);
    }
    else if ((fieldId == "ICMT" || fieldId == "COMM") && metadata.comment.empty())
    {
        metadata.comment = value;
    }
}

void ReadInfoListChunk(ReadContext &context, const bounded::BoundedChunkRange &listChunk, RawMetadata &infoMetadata)
{
    if (listChunk.payloadSize < 4 || listChunk.payloadSize > kMaxListInfoBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> listType = bounded::ReadRangeAt(context, listChunk.payloadOffset, 4, listChunk.payloadEnd, 4);
    if (listType.size() != 4 || std::memcmp(listType.data(), "INFO", 4) != 0)
    {
        return;
    }

    std::uint64_t cursor = listChunk.payloadOffset + 4;
    std::uint64_t childCount = 0;
    while (cursor + kChunkHeaderBytes <= listChunk.payloadEnd && childCount < kMaxRiffChunks)
    {
        ++childCount;
        const std::vector<std::uint8_t> childHeader = bounded::ReadRangeAt(context, cursor, kChunkHeaderBytes, listChunk.payloadEnd, kChunkHeaderBytes);
        if (childHeader.size() != kChunkHeaderBytes)
        {
            break;
        }

        const std::optional<std::uint32_t> childSize = bounded::ReadU32Le(std::span<const std::uint8_t>(childHeader).subspan(4, 4));
        if (!childSize.has_value())
        {
            break;
        }
        const std::optional<bounded::BoundedChunkRange> child = bounded::MakeBoundedChunkRange(cursor + kChunkHeaderBytes, *childSize, listChunk.payloadEnd);
        if (!child.has_value())
        {
            break;
        }

        const std::string_view fieldId(reinterpret_cast<const char *>(childHeader.data()), 4);
        const std::vector<std::uint8_t> valueBytes = bounded::ReadRangeAt(context, child->payloadOffset, child->payloadSize, child->payloadEnd, 1024 * 1024);
        if (valueBytes.size() == child->payloadSize)
        {
            ApplyInfoField(infoMetadata, fieldId, DecodeInfoText(valueBytes));
        }

        cursor = child->paddedEnd;
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

namespace tagreader_riff
{
void ReadRiffWavMetadata(ReadContext &context, RawMetadata &metadata)
{
    std::uint64_t riffEnd = 0;
    if (!ValidateRiffWaveMagic(context, riffEnd))
    {
        return;
    }

    RawMetadata infoMetadata{};
    RawMetadata id3Metadata{};

    std::uint64_t cursor = kRiffHeaderBytes;
    std::uint64_t chunkCount = 0;
    while (cursor + kChunkHeaderBytes <= riffEnd && chunkCount < kMaxRiffChunks)
    {
        ++chunkCount;
        const std::vector<std::uint8_t> chunkHeader = bounded::ReadRangeAt(context, cursor, kChunkHeaderBytes, riffEnd, kChunkHeaderBytes);
        if (chunkHeader.size() != kChunkHeaderBytes)
        {
            break;
        }

        const std::optional<std::uint32_t> chunkSize = bounded::ReadU32Le(std::span<const std::uint8_t>(chunkHeader).subspan(4, 4));
        if (!chunkSize.has_value())
        {
            break;
        }

        const std::optional<bounded::BoundedChunkRange> chunk = bounded::MakeBoundedChunkRange(cursor + kChunkHeaderBytes, *chunkSize, riffEnd);
        if (!chunk.has_value())
        {
            break;
        }

        const std::span<const std::uint8_t> chunkId(chunkHeader.data(), 4);
        if (MatchesFourCc(chunkId, "LIST"))
        {
            ReadInfoListChunk(context, *chunk, infoMetadata);
        }
        else if (MatchesFourCc(chunkId, "id3 ") || MatchesFourCc(chunkId, "ID3 "))
        {
            ReadEmbeddedId3(context, *chunk, id3Metadata);
        }

        cursor = chunk->paddedEnd;
    }

    metadata = std::move(id3Metadata);
    FillMissingMetadata(metadata, infoMetadata);
}
}
