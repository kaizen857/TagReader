#include "formats/flac/FlacParser.hpp"

#include "formats/flac/FlacPicture.hpp"
#include "formats/vorbis/VorbisCommentLimits.hpp"
#include "formats/vorbis/VorbisCommentParser.hpp"
#include "io/ByteReader.hpp"

#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_io::ReadBE24;
using tagreader_io::ReadLE32;
using tagreader_io::ReadRange;
using tagreader_io::TryAddUintmax;

constexpr std::size_t kMaxTextFieldBytes = 1z * 1024 * 1024;
constexpr std::size_t kMaxLyricsBytes = 8z * 1024 * 1024;
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;

bool HasFlacSignature(ReadContext &context)
{
    const std::vector<uint8_t> signature = ReadRange(context.input, 0, 4);
    return signature.size() == 4 && std::string_view(reinterpret_cast<const char *>(signature.data()), 4) == "fLaC";
}

template <typename Handler>
bool ForEachFlacVorbisCommentEntry(const uint8_t *data, std::size_t size, Handler &&handler)
{
    if (data == nullptr || size < 8)
    {
        return false;
    }

    std::size_t cursor = 0;
    const uint32_t vendorLength = ReadLE32(data + cursor);
    cursor += 4;
    if (vendorLength > size - cursor)
    {
        return false;
    }
    cursor += vendorLength;

    if (size - cursor < 4)
    {
        return false;
    }

    const uint32_t commentCount = ReadLE32(data + cursor);
    cursor += 4;
    if (commentCount > tagreader_vorbis::kMaxVorbisComments || commentCount > (size - cursor) / 4)
    {
        return false;
    }

    for (uint32_t i = 0; i < commentCount; ++i)
    {
        if (size - cursor < 4)
        {
            return false;
        }

        const uint32_t commentLength = ReadLE32(data + cursor);
        cursor += 4;
        if (commentLength > size - cursor)
        {
            return false;
        }

        handler(std::string_view(reinterpret_cast<const char *>(data + cursor), commentLength));
        cursor += commentLength;
    }

    return true;
}

void ReadFlacMetadataBlocks(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open() || context.fileSize < 8)
    {
        return;
    }

    std::uintmax_t cursor = 4;
    while (true)
    {
        std::uintmax_t blockHeaderEnd = 0;
        if (!TryAddUintmax(cursor, 4, blockHeaderEnd) || blockHeaderEnd > context.fileSize)
        {
            break;
        }

        const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
        if (blockHeader.size() != 4)
        {
            throw std::runtime_error("failed to read FLAC metadata block header");
        }

        const bool lastBlock = (blockHeader[0] & 0x80) != 0;
        const uint32_t blockType = blockHeader[0] & 0x7F;
        const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
        cursor += 4;

        std::uintmax_t blockEnd = 0;
        if (!TryAddUintmax(cursor, blockSize, blockEnd) || blockEnd > context.fileSize)
        {
            throw std::runtime_error("truncated FLAC metadata block");
        }

        if (blockType == 4)
        {
            if (blockSize > kMaxTextFieldBytes)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }

            const std::vector<uint8_t> block = ReadRange(context.input, cursor, static_cast<std::size_t>(blockSize), kMaxTextFieldBytes);
            if (block.size() != blockSize)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }

            const bool ok = ForEachFlacVorbisCommentEntry(block.data(), block.size(), [&](std::string_view entry)
                                                          { tagreader_vorbis::ReadVorbisCommentEntry(metadata, entry); });
            if (!ok)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }
        }
        else if (blockType == 6)
        {
            if (!metadata.coverPath.empty() || blockSize > kMaxCoverInputBytes)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }

            const std::vector<uint8_t> picture = ReadRange(context.input, cursor, blockSize, kMaxCoverInputBytes);
            if (picture.size() != blockSize)
            {
                throw std::runtime_error("failed to read FLAC picture block");
            }
            tagreader_flac::ReadFlacPictureEntry(context, metadata, picture.data(), picture.size());
        }

        cursor = blockEnd;
        if (lastBlock)
        {
            break;
        }
    }
}
}

namespace tagreader_flac
{
void ReadFlacMetadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    if (!HasFlacSignature(context))
    {
        throw std::runtime_error("invalid FLAC signature");
    }

    ReadFlacMetadataBlocks(context, metadata);
}

void ReadFlacLyrics(ReadContext &context, RawLyrics &lyrics)
{
    if (!context.input.is_open())
    {
        return;
    }

    if (!HasFlacSignature(context))
    {
        return;
    }

    std::uintmax_t cursor = 4;
    while (cursor + 4 <= context.fileSize)
    {
        const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
        if (blockHeader.size() != 4)
        {
            break;
        }

        const bool lastBlock = (blockHeader[0] & 0x80) != 0;
        const uint32_t blockType = blockHeader[0] & 0x7F;
        const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
        cursor += 4;
        std::uintmax_t blockEnd = 0;
        if (!TryAddUintmax(cursor, blockSize, blockEnd) || blockEnd > context.fileSize)
        {
            break;
        }

        if (blockType == 4)
        {
            if (blockSize > kMaxLyricsBytes)
            {
                break;
            }

            const std::vector<uint8_t> block = ReadRange(context.input, cursor, static_cast<std::size_t>(blockSize), kMaxLyricsBytes);
            if (block.size() != blockSize)
            {
                break;
            }

            ForEachFlacVorbisCommentEntry(block.data(), block.size(), [&](std::string_view entry)
                                          {
                const auto eq = entry.find('=');
                if (eq != std::string_view::npos)
                {
                    tagreader_vorbis::ReadVorbisLyricsEntry(lyrics, entry.substr(0, eq), entry.substr(eq + 1));
                } });
        }

        cursor = blockEnd;
        if (lastBlock)
        {
            break;
        }
    }
}
}
