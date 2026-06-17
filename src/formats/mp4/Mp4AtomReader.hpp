#ifndef TAGREADER_FORMATS_MP4_MP4ATOMREADER_HPP
#define TAGREADER_FORMATS_MP4_MP4ATOMREADER_HPP

#include "io/ByteReader.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tagreader_mp4
{
struct Mp4AtomHeader
{
    std::uintmax_t offset{};
    std::uintmax_t headerSize{};
    std::uintmax_t payloadOffset{};
    std::uintmax_t atomEnd{};
    std::uint64_t atomSize{};
    std::string atomType;
};

enum class Mp4PathState
{
    Root,
    Moov,
    Udta,
    Meta,
    Ilst,
};

struct PendingMp4AtomRange
{
    std::uintmax_t offset{};
    std::uintmax_t limit{};
    Mp4PathState state{Mp4PathState::Root};
};

struct Mp4AtomRange
{
    std::uintmax_t offset{};
    std::uintmax_t limit{};
};

enum class ParseStatus
{
    Ok,
    NotFound,
    Malformed,
    ResourceLimit,
};

struct Mp4ItemCallbacks
{
    std::function<void(const Mp4AtomHeader &)> onMetadataItem;
    std::function<void(const Mp4AtomHeader &)> onLyricsItem;
    std::function<void(const Mp4AtomHeader &)> onFreeformLyricsItem;
};

constexpr std::size_t kMaxMp4AtomPayloadBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxMp4Atoms = 100000;

bool AtomTypeIs(std::string_view atomType, std::string_view expected);
bool ReadMp4AtomHeader(tagreader_io::FileInput &input, std::uintmax_t offset, std::uintmax_t limit, bool allowSizeZero, Mp4AtomHeader &atom);
std::optional<std::uintmax_t> FindNextMp4SiblingAfterSizeZero(tagreader_io::FileInput &input, std::uintmax_t offset, std::uintmax_t limit);
std::optional<Mp4AtomRange> ReadMp4MetaChildRange(tagreader_io::FileInput &input, const Mp4AtomHeader &atom);
ParseStatus WalkMp4IlstItems(tagreader_io::FileInput &input, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms, const Mp4ItemCallbacks &callbacks);
std::vector<uint8_t> ReadMp4AtomPayload(tagreader_io::FileInput &input, const Mp4AtomHeader &atom, std::size_t maxPayloadSize);

template <typename Handler>
ParseStatus ForEachMp4ChildAtom(tagreader_io::FileInput &input, std::uintmax_t offset, std::uintmax_t limit, bool allowSizeZero, std::size_t &visitedAtoms, Handler &&handler)
{
    if (limit <= offset)
    {
        return ParseStatus::NotFound;
    }

    std::uintmax_t cursor = offset;
    ParseStatus recoveredStatus = ParseStatus::Ok;
    while (cursor < limit)
    {
        if (++visitedAtoms > kMaxMp4Atoms)
        {
            return ParseStatus::ResourceLimit;
        }

        Mp4AtomHeader atom;
        if (!ReadMp4AtomHeader(input, cursor, limit, allowSizeZero, atom))
        {
            return ParseStatus::Malformed;
        }
        if (atom.payloadOffset > atom.atomEnd || atom.atomEnd > limit)
        {
            return ParseStatus::Malformed;
        }

        if (!handler(atom))
        {
            return ParseStatus::NotFound;
        }

        if (atom.atomSize == 0)
        {
            const std::optional<std::uintmax_t> nextSibling = FindNextMp4SiblingAfterSizeZero(input, atom.payloadOffset, limit);
            if (!nextSibling.has_value())
            {
                return recoveredStatus;
            }
            if (*nextSibling <= cursor || *nextSibling >= limit)
            {
                return ParseStatus::Malformed;
            }

            recoveredStatus = ParseStatus::Malformed;
            cursor = *nextSibling;
            continue;
        }

        const std::uintmax_t nextCursor = atom.atomEnd;
        if (nextCursor <= cursor)
        {
            return ParseStatus::Malformed;
        }
        cursor = nextCursor;
    }

    return recoveredStatus;
}
}

#endif
