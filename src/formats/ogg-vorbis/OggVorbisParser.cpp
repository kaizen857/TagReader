#include "formats/ogg-vorbis/OggVorbisParser.hpp"

#include "formats/vorbis/VorbisCommentParser.hpp"
#include "io/ByteReader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
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

VorbisStreamState &FindState(std::vector<VorbisStreamState> &states, std::uint32_t serial)
{
    auto it = std::find_if(states.begin(), states.end(), [serial](const VorbisStreamState &state)
                           { return state.serial == serial; });
    if (it == states.end())
    {
        states.push_back(VorbisStreamState{.serial = serial});
        return states.back();
    }
    return *it;
}

bool HasVorbisPrefix(const std::vector<uint8_t> &bytes, uint8_t packetType)
{
    return bytes.size() >= 7 && bytes[0] == packetType && std::string_view(reinterpret_cast<const char *>(bytes.data() + 1), 6) == "vorbis";
}

bool ReadOggVorbisCommentEntries(ReadContext &context, const std::function<void(std::string_view)> &handler)
{
    if (!context.input.is_open())
    {
        return false;
    }

    std::uintmax_t cursor = 0;
    std::vector<VorbisStreamState> states;
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
        VorbisStreamState &state = FindState(states, serial);
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
                if (state.stage == VorbisStreamStage::LookingForIdentification && HasVorbisPrefix(state.packet, 0x01))
                {
                    state.stage = VorbisStreamStage::LookingForComment;
                }
                else if (state.stage == VorbisStreamStage::LookingForComment && HasVorbisPrefix(state.packet, 0x03))
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
    const bool ok = ReadOggVorbisCommentEntries(context, [&](std::string_view entry)
                                                { tagreader_vorbis::ReadVorbisCommentEntry(metadata, entry); });
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
