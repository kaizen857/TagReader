#include "formats/dsd/DsdParser.hpp"

#include "formats/common/BoundedReader.hpp"
#include "formats/id3/Id3Parser.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
namespace bounded = tagreader_core::formats;

constexpr std::uint64_t kDsfHeaderBytes = 28;
constexpr std::uint64_t kDffHeaderBytes = 16;
constexpr std::uint64_t kDffChunkHeaderBytes = 12;
constexpr std::uint64_t kMaxId3PayloadBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxDffChunks = 100000;
constexpr unsigned kMaxDffChunkDepth = 8;

bool MatchesFourCc(std::span<const std::uint8_t> bytes, std::string_view fourCc)
{
    return bytes.size() == 4 && fourCc.size() == 4 &&
           std::memcmp(bytes.data(), fourCc.data(), 4) == 0;
}

bool ValidateDsfMagicAndReadMetadataPointer(ReadContext &context, std::uint64_t &metadataOffset, std::uint64_t &metadataEnd)
{
    const std::vector<std::uint8_t> header = bounded::ReadRangeAt(context, 0, kDsfHeaderBytes, context.fileSize, kDsfHeaderBytes);
    if (header.size() != kDsfHeaderBytes || std::memcmp(header.data(), "DSD ", 4) != 0)
    {
        return false;
    }

    const std::optional<std::uint64_t> headerSize = bounded::ReadU64Le(std::span<const std::uint8_t>(header).subspan(4, 8));
    const std::optional<std::uint64_t> fileSize = bounded::ReadU64Le(std::span<const std::uint8_t>(header).subspan(12, 8));
    const std::optional<std::uint64_t> pointer = bounded::ReadU64Le(std::span<const std::uint8_t>(header).subspan(20, 8));
    if (!headerSize.has_value() || !fileSize.has_value() || !pointer.has_value() || *headerSize != kDsfHeaderBytes)
    {
        return false;
    }
    if (*fileSize != 0 && *fileSize > context.fileSize)
    {
        return false;
    }

    metadataOffset = *pointer;
    metadataEnd = *fileSize == 0 ? context.fileSize : *fileSize;
    return true;
}

void ReadId3Payload(ReadContext &context, std::uint64_t payloadOffset, std::uint64_t payloadSize, std::uint64_t payloadEnd, RawMetadata &metadata)
{
    if (payloadSize == 0 || payloadSize > kMaxId3PayloadBytes)
    {
        return;
    }

    const std::vector<std::uint8_t> id3Bytes = bounded::ReadRangeAt(context, payloadOffset, payloadSize, payloadEnd, kMaxId3PayloadBytes);
    if (id3Bytes.size() != payloadSize)
    {
        return;
    }

    tagreader_id3::ReadID3v2MetadataFromBytes(context, id3Bytes, metadata);
}

bool ValidateDffMagic(ReadContext &context, std::uint64_t &formEnd)
{
    const std::vector<std::uint8_t> header = bounded::ReadRangeAt(context, 0, kDffHeaderBytes, context.fileSize, kDffHeaderBytes);
    if (header.size() != kDffHeaderBytes || std::memcmp(header.data(), "FRM8", 4) != 0 || std::memcmp(header.data() + 12, "DSD ", 4) != 0)
    {
        return false;
    }

    const std::optional<std::uint64_t> formPayloadSize = bounded::ReadU64Be(std::span<const std::uint8_t>(header).subspan(4, 8));
    if (!formPayloadSize.has_value())
    {
        return false;
    }

    const std::optional<bounded::BoundedRange> formRange = bounded::MakeBoundedRange(12, *formPayloadSize, context.fileSize);
    if (!formRange.has_value() || formRange->end < kDffHeaderBytes)
    {
        return false;
    }

    formEnd = formRange->end;
    return true;
}

bool ChunkMayContainChildren(std::span<const std::uint8_t> chunkId)
{
    return MatchesFourCc(chunkId, "PROP") || MatchesFourCc(chunkId, "FRM8") || MatchesFourCc(chunkId, "LIST");
}

bool ScanDffChunksForId3(ReadContext &context, std::uint64_t begin, std::uint64_t end, unsigned depth, RawMetadata &metadata)
{
    if (depth > kMaxDffChunkDepth)
    {
        return false;
    }

    std::uint64_t cursor = begin;
    std::uint64_t chunkCount = 0;
    while (cursor + kDffChunkHeaderBytes <= end && chunkCount < kMaxDffChunks)
    {
        ++chunkCount;
        const std::vector<std::uint8_t> chunkHeader = bounded::ReadRangeAt(context, cursor, kDffChunkHeaderBytes, end, kDffChunkHeaderBytes);
        if (chunkHeader.size() != kDffChunkHeaderBytes)
        {
            break;
        }

        const std::optional<std::uint64_t> chunkSize = bounded::ReadU64Be(std::span<const std::uint8_t>(chunkHeader).subspan(4, 8));
        if (!chunkSize.has_value())
        {
            break;
        }

        const std::optional<bounded::BoundedChunkRange> chunk = bounded::MakeBoundedChunkRange(cursor + kDffChunkHeaderBytes, *chunkSize, end, 2);
        if (!chunk.has_value())
        {
            break;
        }

        const std::span<const std::uint8_t> chunkId(chunkHeader.data(), 4);
        if (MatchesFourCc(chunkId, "ID3 ") || MatchesFourCc(chunkId, "DI3v"))
        {
            ReadId3Payload(context, chunk->payloadOffset, chunk->payloadSize, chunk->payloadEnd, metadata);
            return true;
        }
        if (ChunkMayContainChildren(chunkId) && chunk->payloadSize >= 4 &&
            ScanDffChunksForId3(context, chunk->payloadOffset + 4, chunk->payloadEnd, depth + 1, metadata))
        {
            return true;
        }

        cursor = chunk->paddedEnd;
    }

    return false;
}
}

namespace tagreader_dsd
{
void ReadDsfMetadata(ReadContext &context, RawMetadata &metadata)
{
    std::uint64_t metadataOffset = 0;
    std::uint64_t metadataEnd = 0;
    if (!ValidateDsfMagicAndReadMetadataPointer(context, metadataOffset, metadataEnd) || metadataOffset == 0 || metadataOffset >= metadataEnd)
    {
        return;
    }

    ReadId3Payload(context, metadataOffset, metadataEnd - metadataOffset, metadataEnd, metadata);
}

void ReadDffMetadata(ReadContext &context, RawMetadata &metadata)
{
    std::uint64_t formEnd = 0;
    if (!ValidateDffMagic(context, formEnd))
    {
        return;
    }

    (void)ScanDffChunksForId3(context, kDffHeaderBytes, formEnd, 0, metadata);
}
}
