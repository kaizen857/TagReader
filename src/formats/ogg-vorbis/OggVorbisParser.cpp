#include "formats/ogg-vorbis/OggVorbisParser.hpp"
#include "profiling/Profiling.hpp"

#include "formats/flac/FlacPicture.hpp"
#include "profiling/Profiling.hpp"
#include "formats/vorbis/VorbisCommentLimits.hpp"
#include "profiling/Profiling.hpp"
#include "formats/vorbis/VorbisCommentParser.hpp"
#include "profiling/Profiling.hpp"
#include "io/ByteReader.hpp"
#include "profiling/Profiling.hpp"

#include <array>
#include "profiling/Profiling.hpp"
#include <cstddef>
#include "profiling/Profiling.hpp"
#include <cstdint>
#include "profiling/Profiling.hpp"
#include <functional>
#include <optional>
#include <string>
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

enum class VorbisStreamStage
{
    LookingForIdentification,
    LookingForComment,
    Done,
    Rejected,
};

struct VorbisStreamState
{
    std::uint32_t serial{};
    std::uint32_t expectedSequence{};
    bool hasSequence{};
    VorbisStreamStage stage{VorbisStreamStage::LookingForIdentification};
    std::vector<uint8_t> packet;
};

template <typename Handler>
bool ForEachVorbisCommentEntry(const uint8_t *data, std::size_t size, Handler &&handler)
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

bool HasVorbisPrefix(const std::vector<uint8_t> &bytes, uint8_t packetType)
{
    return bytes.size() >= 7 && bytes[0] == packetType && std::string_view(reinterpret_cast<const char *>(bytes.data() + 1), 6) == "vorbis";
}

bool IsValidVorbisIdentificationPacket(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < 30 || !HasVorbisPrefix(bytes, 0x01))
    {
        return false;
    }

    const std::uint32_t version = ReadLE32(bytes.data() + 7);
    const uint8_t channels = bytes[11];
    const std::uint32_t sampleRate = ReadLE32(bytes.data() + 12);
    const bool hasFramingFlag = (bytes[29] & 0x01) != 0;
    return version == 0 && channels > 0 && sampleRate > 0 && hasFramingFlag;
}

bool IsPlausibleVorbisCommentPacket(const std::vector<uint8_t> &bytes)
{
    return bytes.size() >= 11 && HasVorbisPrefix(bytes, 0x03);
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

void ReadOggVorbisPictureEntry(ReadContext &context, RawMetadata &metadata, std::string_view entry)
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

bool ReadOggVorbisCommentEntries(ReadContext &context, const std::function<void(std::string_view)> &handler)
{
    if (!context.input.is_open())
    {
        return false;
    }

    std::uintmax_t cursor = 0;
    std::unordered_map<std::uint32_t, VorbisStreamState> states;
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
            stateIt = states.try_emplace(serial, VorbisStreamState{.serial = serial}).first;
        }
        VorbisStreamState &state = stateIt->second;
        if (!state.hasSequence)
        {
            state.hasSequence = true;
            state.expectedSequence = sequence;
        }
        else if (sequence != state.expectedSequence + 1)
        {
            state.stage = VorbisStreamStage::Rejected;
            state.packet.clear();
        }
        state.expectedSequence = sequence;
        if (!state.packet.empty() && !continuation)
        {
            state.stage = VorbisStreamStage::Rejected;
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

        if (state.stage == VorbisStreamStage::Rejected || state.stage == VorbisStreamStage::Done)
        {
            cursor = nextCursor;
            continue;
        }
        if (continuation && state.packet.empty())
        {
            state.stage = VorbisStreamStage::Rejected;
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
                // Ogg packets can span pages; only classify Vorbis packets after lacing closes them.
                if (state.stage == VorbisStreamStage::LookingForIdentification && IsValidVorbisIdentificationPacket(state.packet))
                {
                    state.stage = VorbisStreamStage::LookingForComment;
                }
                else if (state.stage == VorbisStreamStage::LookingForComment && IsPlausibleVorbisCommentPacket(state.packet))
                {
                    const uint8_t *commentData = state.packet.data() + 7;
                    const std::size_t commentSize = state.packet.size() - 7;
                    const bool ok = ForEachVorbisCommentEntry(commentData, commentSize, [&](std::string_view entry)
                                                              { handler(entry); });
                    state.stage = VorbisStreamStage::Done;
                    return ok;
                }
                else if (state.stage == VorbisStreamStage::LookingForIdentification)
                {
                    state.stage = VorbisStreamStage::Rejected;
                }

                state.packet.clear();
            }
        }

        cursor = nextCursor;
    }

    return false;
}
}

namespace tagreader_ogg_vorbis
{
void ReadOggVorbisMetadata(ReadContext &context, RawMetadata &metadata)
{
    TAGREADER_PROFILE_FUNCTION();
    
    const bool ok = ReadOggVorbisCommentEntries(context, [&](std::string_view entry)
                                                {
        tagreader_vorbis::ReadVorbisCommentEntry(metadata, entry);
        ReadOggVorbisPictureEntry(context, metadata, entry); });
    (void)ok;
}

void ReadOggVorbisLyrics(ReadContext &context, RawLyrics &lyrics)
{
    const bool ok = ReadOggVorbisCommentEntries(context, [&](std::string_view entry)
                                                {
        const auto eq = entry.find('=');
        if (eq != std::string_view::npos)
        {
            tagreader_vorbis::ReadVorbisLyricsEntry(lyrics, entry.substr(0, eq), entry.substr(eq + 1));
        } });
    (void)ok;
}
}
