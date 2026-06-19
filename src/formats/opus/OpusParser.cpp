#include "formats/opus/OpusParser.hpp"

#include "formats/flac/FlacPicture.hpp"
#include "formats/vorbis/VorbisCommentLimits.hpp"
#include "formats/vorbis/VorbisCommentParser.hpp"
#include "io/ByteReader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_io::ReadLE32;
using tagreader_io::ReadRange;
using tagreader_io::TryAddUintmax;

constexpr std::size_t kMaxOggPacketBytes = 8z * 1024 * 1024;
constexpr std::size_t kMaxOggScannedBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxOggPages = 100000;
constexpr std::size_t kMaxOggLogicalStreams = 256;
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;

enum class OpusStreamStage
{
    LookingForHead,
    LookingForTags,
    Done,
    Rejected,
};

struct OpusStreamState
{
    std::uint32_t serial{};
    std::uint32_t expectedSequence{};
    bool hasSequence{};
    OpusStreamStage stage{OpusStreamStage::LookingForHead};
    std::vector<uint8_t> packet;
};

template <typename Handler>
bool ForEachOpusTagEntry(const uint8_t *data, std::size_t size, Handler &&handler)
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

bool HasOpusPrefix(const std::vector<uint8_t> &bytes, std::string_view prefix)
{
    return bytes.size() >= prefix.size() && std::string_view(reinterpret_cast<const char *>(bytes.data()), prefix.size()) == prefix;
}

bool IsValidOpusHeadPacket(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < 19 || !HasOpusPrefix(bytes, "OpusHead"))
    {
        return false;
    }

    const uint8_t version = bytes[8];
    const uint8_t channels = bytes[9];
    return (version & 0xF0) == 0 && channels > 0;
}

bool IsOpusTagsPacket(const std::vector<uint8_t> &bytes)
{
    return HasOpusPrefix(bytes, "OpusTags");
}

std::optional<uint8_t> DecodeBase64Char(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return static_cast<uint8_t>(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z')
    {
        return static_cast<uint8_t>(26 + ch - 'a');
    }
    if (ch >= '0' && ch <= '9')
    {
        return static_cast<uint8_t>(52 + ch - '0');
    }
    if (ch == '+')
    {
        return 62;
    }
    if (ch == '/')
    {
        return 63;
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> DecodeBase64(std::string_view text, std::size_t maxDecodedBytes)
{
    if (text.empty() || text.size() % 4 != 0)
    {
        return std::nullopt;
    }

    const std::size_t padding = (text.ends_with("==") ? 2z : (text.ends_with('=') ? 1z : 0z));
    if (padding > 0)
    {
        for (std::size_t i = text.size() - padding; i < text.size(); ++i)
        {
            if (text[i] != '=')
            {
                return std::nullopt;
            }
        }
    }
    for (std::size_t i = 0; i + padding < text.size(); ++i)
    {
        if (text[i] == '=')
        {
            return std::nullopt;
        }
    }

    const std::size_t decodedSize = (text.size() / 4) * 3 - padding;
    if (decodedSize > maxDecodedBytes)
    {
        return std::nullopt;
    }

    std::vector<uint8_t> decoded;
    decoded.reserve(decodedSize);
    for (std::size_t offset = 0; offset < text.size(); offset += 4)
    {
        std::array<uint8_t, 4> values{};
        std::size_t blockPadding = 0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const char ch = text[offset + i];
            if (ch == '=')
            {
                values[i] = 0;
                ++blockPadding;
            }
            else
            {
                const std::optional<uint8_t> value = DecodeBase64Char(static_cast<unsigned char>(ch));
                if (!value.has_value() || blockPadding != 0)
                {
                    return std::nullopt;
                }
                values[i] = *value;
            }
        }

        if (blockPadding > 0 && offset + 4 != text.size())
        {
            return std::nullopt;
        }
        if (blockPadding > 2)
        {
            return std::nullopt;
        }

        decoded.push_back(static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)));
        if (blockPadding < 2)
        {
            decoded.push_back(static_cast<uint8_t>((values[1] << 4) | (values[2] >> 2)));
        }
        if (blockPadding < 1)
        {
            decoded.push_back(static_cast<uint8_t>((values[2] << 6) | values[3]));
        }
    }

    if (decoded.size() != decodedSize)
    {
        return std::nullopt;
    }
    return decoded;
}

void ReadOpusPictureEntry(ReadContext &context, RawMetadata &metadata, std::string_view entry)
{
    if (!metadata.coverPath.empty())
    {
        return;
    }

    constexpr std::string_view kPictureKey = "METADATA_BLOCK_PICTURE=";
    if (!entry.starts_with(kPictureKey))
    {
        return;
    }

    const std::optional<std::vector<uint8_t>> picture = DecodeBase64(entry.substr(kPictureKey.size()), kMaxCoverInputBytes);
    if (!picture.has_value())
    {
        return;
    }

    tagreader_flac::ReadFlacPictureEntry(context, metadata, picture->data(), picture->size());
}

bool ReadOggOpusTagEntries(ReadContext &context, const std::function<void(std::string_view)> &handler)
{
    if (!context.input.is_open())
    {
        return false;
    }

    std::uintmax_t cursor = 0;
    std::unordered_map<std::uint32_t, OpusStreamState> states;
    std::size_t totalScannedBytes = 0;
    std::size_t pageCount = 0;
    while (true)
    {
        if (++pageCount > kMaxOggPages)
        {
            return false;
        }

        std::uintmax_t pageHeaderEnd = 0;
        if (!TryAddUintmax(cursor, 27, pageHeaderEnd) || pageHeaderEnd > context.fileSize)
        {
            break;
        }

        const std::vector<uint8_t> pageHeader = ReadRange(context.input, cursor, 27);
        if (pageHeader.size() != 27 || std::string_view(reinterpret_cast<const char *>(pageHeader.data()), 4) != "OggS" || pageHeader[4] != 0)
        {
            return false;
        }

        const bool continuation = (pageHeader[5] & 0x01) != 0;
        const std::uint32_t serial = ReadLE32(pageHeader.data() + 14);
        const std::uint32_t sequence = ReadLE32(pageHeader.data() + 18);
        auto stateIt = states.find(serial);
        if (stateIt == states.end())
        {
            if (states.size() >= kMaxOggLogicalStreams)
            {
                return false;
            }
            stateIt = states.try_emplace(serial, OpusStreamState{.serial = serial}).first;
        }
        OpusStreamState &state = stateIt->second;
        if (!state.hasSequence)
        {
            state.hasSequence = true;
            state.expectedSequence = sequence;
        }
        else if (sequence != state.expectedSequence + 1)
        {
            state.stage = OpusStreamStage::Rejected;
            state.packet.clear();
        }
        state.expectedSequence = sequence;
        if (!state.packet.empty() && !continuation)
        {
            state.stage = OpusStreamStage::Rejected;
            state.packet.clear();
        }

        const uint8_t segmentCount = pageHeader[26];
        std::uintmax_t segmentTableOffset = 0;
        if (!TryAddUintmax(cursor, 27, segmentTableOffset))
        {
            return false;
        }

        const std::vector<uint8_t> segmentTable = ReadRange(context.input, segmentTableOffset, segmentCount);
        if (segmentTable.size() != segmentCount)
        {
            return false;
        }

        std::size_t payloadSize = 0;
        for (uint8_t seg : segmentTable)
        {
            payloadSize += seg;
        }
        if (payloadSize > kMaxOggPacketBytes)
        {
            return false;
        }

        std::uintmax_t payloadOffset = 0;
        if (!TryAddUintmax(segmentTableOffset, segmentCount, payloadOffset))
        {
            return false;
        }

        std::uintmax_t nextCursor = 0;
        if (!TryAddUintmax(payloadOffset, payloadSize, nextCursor))
        {
            return false;
        }
        if (nextCursor > context.fileSize)
        {
            return false;
        }
        const std::uintmax_t scannedDelta = nextCursor - cursor;
        if (scannedDelta > kMaxOggScannedBytes || totalScannedBytes > kMaxOggScannedBytes - static_cast<std::size_t>(scannedDelta))
        {
            return false;
        }
        totalScannedBytes += static_cast<std::size_t>(scannedDelta);

        const std::vector<uint8_t> payload = ReadRange(context.input, payloadOffset, payloadSize, kMaxOggPacketBytes);
        if (payload.size() != payloadSize)
        {
            return false;
        }

        if (state.stage == OpusStreamStage::Rejected || state.stage == OpusStreamStage::Done)
        {
            cursor = nextCursor;
            continue;
        }
        if (continuation && state.packet.empty())
        {
            state.stage = OpusStreamStage::Rejected;
            cursor = nextCursor;
            continue;
        }

        std::size_t payloadCursor = 0;
        for (uint8_t segmentSize : segmentTable)
        {
            if (payloadCursor + segmentSize > payload.size())
            {
                return false;
            }

            if (segmentSize > kMaxOggPacketBytes - state.packet.size())
            {
                return false;
            }

            state.packet.insert(state.packet.end(), payload.begin() + static_cast<std::ptrdiff_t>(payloadCursor), payload.begin() + static_cast<std::ptrdiff_t>(payloadCursor + segmentSize));
            payloadCursor += segmentSize;

            if (segmentSize < 255)
            {
                if (state.stage == OpusStreamStage::LookingForHead && IsValidOpusHeadPacket(state.packet))
                {
                    state.stage = OpusStreamStage::LookingForTags;
                }
                else if (state.stage == OpusStreamStage::LookingForTags && IsOpusTagsPacket(state.packet))
                {
                    const uint8_t *tagData = state.packet.data() + 8;
                    const std::size_t tagSize = state.packet.size() - 8;
                    const bool ok = ForEachOpusTagEntry(tagData, tagSize, [&](std::string_view entry)
                                                       { handler(entry); });
                    state.stage = OpusStreamStage::Done;
                    return ok;
                }
                else
                {
                    state.stage = OpusStreamStage::Rejected;
                }

                state.packet.clear();
            }
        }

        cursor = nextCursor;
    }

    return false;
}
}

namespace tagreader_opus
{
void ReadOggOpusMetadata(ReadContext &context, RawMetadata &metadata)
{
    const bool ok = ReadOggOpusTagEntries(context, [&](std::string_view entry)
                                          {
        tagreader_vorbis::ReadVorbisCommentEntry(metadata, entry);
        ReadOpusPictureEntry(context, metadata, entry); });
    (void)ok;
}

void ReadOggOpusLyrics(ReadContext &context, RawLyrics &lyrics)
{
    const bool ok = ReadOggOpusTagEntries(context, [&](std::string_view entry)
                                          {
        const auto eq = entry.find('=');
        if (eq != std::string_view::npos)
        {
            tagreader_vorbis::ReadVorbisLyricsEntry(lyrics, entry.substr(0, eq), entry.substr(eq + 1));
        } });
    (void)ok;
}
}
