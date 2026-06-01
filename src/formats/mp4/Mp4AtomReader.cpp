#include "formats/mp4/Mp4AtomReader.hpp"

#include "io/ByteReader.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace tagreader_mp4
{
using tagreader_io::ReadBE32;
using tagreader_io::ReadRange;
using tagreader_io::TryAddUintmax;

namespace
{
bool IsSupportedMp4MetadataItem(std::string_view atomType)
{
    constexpr std::array<char, 4> kMp4TitleAtom{static_cast<char>(0xA9), 'n', 'a', 'm'};
    constexpr std::array<char, 4> kMp4ArtistAtom{static_cast<char>(0xA9), 'A', 'R', 'T'};
    constexpr std::array<char, 4> kMp4AlbumAtom{static_cast<char>(0xA9), 'a', 'l', 'b'};
    constexpr std::array<char, 4> kMp4ComposerAtom{static_cast<char>(0xA9), 'w', 'r', 't'};
    constexpr std::array<char, 4> kMp4GenreAtom{static_cast<char>(0xA9), 'g', 'e', 'n'};
    constexpr std::array<char, 4> kMp4DayAtom{static_cast<char>(0xA9), 'd', 'a', 'y'};

    return AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) ||
           AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) ||
           AtomTypeIs(atomType, "aART") ||
           AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) ||
           AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) ||
           AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) ||
           AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) ||
           AtomTypeIs(atomType, "date") ||
           AtomTypeIs(atomType, "trkn") ||
           AtomTypeIs(atomType, "disk") ||
           AtomTypeIs(atomType, "covr");
}
}

bool AtomTypeIs(std::string_view atomType, std::string_view expected)
{
    return atomType.size() == expected.size() && std::memcmp(atomType.data(), expected.data(), expected.size()) == 0;
}

bool ReadMp4AtomHeader(std::ifstream &input, std::uintmax_t offset, std::uintmax_t limit, bool allowSizeZero, Mp4AtomHeader &atom)
{
    atom = {};
    if (limit <= offset)
    {
        return false;
    }

    std::uintmax_t basicHeaderEnd = 0;
    if (!TryAddUintmax(offset, 8, basicHeaderEnd) || basicHeaderEnd > limit)
    {
        return false;
    }

    const std::vector<uint8_t> header = ReadRange(input, offset, 8);
    if (header.size() != 8)
    {
        return false;
    }

    uint64_t atomSize = ReadBE32(header.data());
    std::string atomType(reinterpret_cast<const char *>(header.data() + 4), 4);
    std::uintmax_t headerSize = 8;
    std::uintmax_t payloadOffset = basicHeaderEnd;

    if (atomSize == 1)
    {
        std::uintmax_t extHeaderEnd = 0;
        if (!TryAddUintmax(offset, 16, extHeaderEnd) || extHeaderEnd > limit)
        {
            return false;
        }

        const std::vector<uint8_t> ext = ReadRange(input, basicHeaderEnd, 8);
        if (ext.size() != 8)
        {
            return false;
        }

        atomSize = (static_cast<uint64_t>(ReadBE32(ext.data())) << 32) | ReadBE32(ext.data() + 4);
        headerSize = 16;
        payloadOffset = extHeaderEnd;
    }

    std::uintmax_t atomEnd = limit;
    if (atomSize == 0)
    {
        if (!allowSizeZero)
        {
            return false;
        }
    }
    else
    {
        if (atomSize < headerSize)
        {
            return false;
        }
        if (atomSize > std::numeric_limits<std::uintmax_t>::max())
        {
            return false;
        }
        if (!TryAddUintmax(offset, static_cast<std::uintmax_t>(atomSize), atomEnd) || atomEnd > limit)
        {
            return false;
        }
    }

    atom.offset = offset;
    atom.headerSize = headerSize;
    atom.payloadOffset = payloadOffset;
    atom.atomEnd = atomEnd;
    atom.atomSize = atomSize;
    atom.atomType = std::move(atomType);
    return true;
}

std::optional<std::uintmax_t> FindNextMp4SiblingAfterSizeZero(std::ifstream &input, std::uintmax_t offset, std::uintmax_t limit)
{
    (void)input;
    (void)offset;
    (void)limit;
    return std::nullopt;
}

std::optional<Mp4AtomRange> ReadMp4MetaChildRange(std::ifstream &input, const Mp4AtomHeader &atom)
{
    if (atom.payloadOffset > atom.atomEnd)
    {
        return std::nullopt;
    }

    const std::uintmax_t payloadSize = atom.atomEnd - atom.payloadOffset;
    if (payloadSize < 4)
    {
        return std::nullopt;
    }

    const std::vector<uint8_t> fullBox = ReadRange(input, atom.payloadOffset, 4, 4);
    if (fullBox.size() != 4)
    {
        return std::nullopt;
    }

    const uint8_t version = fullBox[0];
    const uint32_t flags = (static_cast<uint32_t>(fullBox[1]) << 16) | (static_cast<uint32_t>(fullBox[2]) << 8) | fullBox[3];
    (void)flags;
    if (version != 0)
    {
        return std::nullopt;
    }

    std::uintmax_t childOffset = 0;
    if (!TryAddUintmax(atom.payloadOffset, 4, childOffset) || childOffset > atom.atomEnd)
    {
        return std::nullopt;
    }

    return Mp4AtomRange{childOffset, atom.atomEnd};
}

ParseStatus WalkMp4IlstItems(std::ifstream &input, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms, const Mp4ItemCallbacks &callbacks)
{
    if (!input.is_open() || limit <= offset)
    {
        return ParseStatus::NotFound;
    }

    std::vector<PendingMp4AtomRange> stack;
    stack.push_back(PendingMp4AtomRange{offset, limit, Mp4PathState::Root});

    while (!stack.empty())
    {
        const PendingMp4AtomRange range = stack.back();
        stack.pop_back();
        std::vector<PendingMp4AtomRange> childRanges;

        const ParseStatus status = ForEachMp4ChildAtom(input, range.offset, range.limit, true, visitedAtoms, [&](const Mp4AtomHeader &atom)
                                                       {
            if (range.state == Mp4PathState::Ilst)
            {
                constexpr std::array<char, 4> kMp4LyricsAtom{static_cast<char>(0xA9), 'l', 'y', 'r'};
                if (callbacks.onMetadataItem && IsSupportedMp4MetadataItem(atom.atomType))
                {
                    callbacks.onMetadataItem(atom);
                }
                if (callbacks.onLyricsItem && AtomTypeIs(atom.atomType, std::string_view(kMp4LyricsAtom.data(), kMp4LyricsAtom.size())))
                {
                    callbacks.onLyricsItem(atom);
                }
                else if (callbacks.onFreeformLyricsItem && atom.atomType == "----")
                {
                    callbacks.onFreeformLyricsItem(atom);
                }
                return true;
            }

            Mp4PathState childState = range.state;
            std::uintmax_t childOffset = atom.payloadOffset;
            bool descend = false;

            if (range.state == Mp4PathState::Root && atom.atomType == "moov")
            {
                childState = Mp4PathState::Moov;
                descend = true;
            }
            else if (range.state == Mp4PathState::Moov && atom.atomType == "udta")
            {
                childState = Mp4PathState::Udta;
                descend = true;
            }
            else if (range.state == Mp4PathState::Udta && atom.atomType == "meta")
            {
                const std::optional<Mp4AtomRange> childRange = ReadMp4MetaChildRange(input, atom);
                if (!childRange.has_value())
                {
                    return true;
                }
                childOffset = childRange->offset;
                childState = Mp4PathState::Meta;
                descend = true;
            }
            else if (range.state == Mp4PathState::Meta && atom.atomType == "ilst")
            {
                childState = Mp4PathState::Ilst;
                descend = true;
            }

            if (descend && atom.atomSize != 0 && childOffset < atom.atomEnd)
            {
                childRanges.push_back(PendingMp4AtomRange{childOffset, atom.atomEnd, childState});
            }
            return true; });

        if (status == ParseStatus::Malformed || status == ParseStatus::ResourceLimit)
        {
            return status;
        }

        for (auto it = childRanges.rbegin(); it != childRanges.rend(); ++it)
        {
            stack.push_back(*it);
        }
    }

    return ParseStatus::Ok;
}

std::vector<uint8_t> ReadMp4AtomPayload(std::ifstream &input, const Mp4AtomHeader &atom, std::size_t maxPayloadSize)
{
    if (atom.payloadOffset > atom.atomEnd)
    {
        return {};
    }

    const std::uintmax_t payloadSize = atom.atomEnd - atom.payloadOffset;
    if (payloadSize > maxPayloadSize || payloadSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        return {};
    }

    return ReadRange(input, atom.payloadOffset, static_cast<std::size_t>(payloadSize), maxPayloadSize);
}
}
