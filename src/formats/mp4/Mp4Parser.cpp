#include "formats/mp4/Mp4Parser.hpp"
#include "profiling/Profiling.hpp"

#include "common/ParseHelpers.hpp"
#include "cover/CoverCache.hpp"
#include "formats/mp4/Mp4AtomReader.hpp"
#include "io/ByteReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using tagreader_common::ParseYearOnly;
using tagreader_common::ToLower;
using tagreader_core::DecodedField;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_cover::WriteCoverAsPng;
using tagreader_io::ReadBE16;
using tagreader_io::ReadBE32;
using tagreader_mp4::AtomTypeIs;
using tagreader_mp4::ForEachMp4ChildAtom;
using tagreader_mp4::Mp4AtomHeader;
using tagreader_mp4::Mp4ItemCallbacks;
using tagreader_mp4::ParseStatus;
using tagreader_mp4::ReadMp4AtomPayload;
using tagreader_mp4::WalkMp4IlstItems;
using tagreader_text::DecodeRawText;
using tagreader_text::DecodeTextToUtf8;
using tagreader_text::ReadLyricsFromPlainText;
using tagreader_text::TrimText;

constexpr std::size_t kMaxTextFieldBytes = 1z * 1024 * 1024;
constexpr std::size_t kMaxLyricsBytes = 8z * 1024 * 1024;
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;

constexpr std::array<char, 4> kMp4TitleAtom{static_cast<char>(0xA9), 'n', 'a', 'm'};
constexpr std::array<char, 4> kMp4ArtistAtom{static_cast<char>(0xA9), 'A', 'R', 'T'};
constexpr std::array<char, 4> kMp4AlbumAtom{static_cast<char>(0xA9), 'a', 'l', 'b'};
constexpr std::array<char, 4> kMp4ComposerAtom{static_cast<char>(0xA9), 'w', 'r', 't'};
constexpr std::array<char, 4> kMp4GenreAtom{static_cast<char>(0xA9), 'g', 'e', 'n'};
constexpr std::array<char, 4> kMp4DayAtom{static_cast<char>(0xA9), 'd', 'a', 'y'};
constexpr std::array<char, 4> kMp4DateAtom{'d', 'a', 't', 'e'};
constexpr std::array<char, 4> kMp4LyricsAtom{static_cast<char>(0xA9), 'l', 'y', 'r'};

std::size_t Mp4MetadataPayloadLimit(std::string_view atomType)
{
    if (atomType == "covr")
    {
        return kMaxCoverInputBytes;
    }
    if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) || AtomTypeIs(atomType, "aART") || AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
    {
        return kMaxTextFieldBytes;
    }

    return tagreader_mp4::kMaxMp4AtomPayloadBytes;
}

DecodedField DecodeMp4TextData(std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize == 0 || payloadSize > kMaxTextFieldBytes)
    {
        return {};
    }

    const std::string_view raw(reinterpret_cast<const char *>(payload), payloadSize);
    if (dataType == 1)
    {
        return DecodeTextToUtf8(raw, "utf-8");
    }
    if (dataType == 0)
    {
        return DecodeTextToUtf8(raw, "utf-8");
    }
    if (dataType == 2)
    {
        if (payloadSize % 2 != 0)
        {
            return {};
        }
        return DecodeTextToUtf8(raw, "utf-16be");
    }
    if (dataType == 3)
    {
        if (payloadSize % 2 != 0)
        {
            return {};
        }
        return DecodeTextToUtf8(raw, "utf-16le");
    }

    return {};
}

std::optional<std::uint16_t> ParseMp4TrackDiskNumber(const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize < 6 || ReadBE16(payload) != 0)
    {
        return std::nullopt;
    }

    const std::uint16_t index = static_cast<std::uint16_t>(ReadBE16(payload + 2));
    if (index == 0)
    {
        return std::nullopt;
    }

    (void)ReadBE16(payload + 4);
    return index;
}

void ReadMp4DataAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize == 0)
    {
        return;
    }

    if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) || AtomTypeIs(atomType, "aART") || AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
    {
        const DecodedField field = DecodeMp4TextData(dataType, payload, payloadSize);
        if (!field.success)
        {
            return;
        }

        const std::string value = field.value;
        if (value.empty())
        {
            return;
        }

        if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) && metadata.title.empty())
            metadata.title = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) && metadata.artist.empty())
            metadata.artist = value;
        else if (atomType == "aART" && metadata.albumArtist.empty())
            metadata.albumArtist = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) && metadata.album.empty())
            metadata.album = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) && metadata.composer.empty())
            metadata.composer = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) && metadata.genre.empty())
            metadata.genre = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
            metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
        return;
    }

    if (atomType == "trkn")
    {
        if (dataType != 0 && dataType != 21)
        {
            return;
        }
        const std::optional<std::uint16_t> trackNumber = ParseMp4TrackDiskNumber(payload, payloadSize);
        if (metadata.trackNumber == 0 && trackNumber.has_value())
        {
            metadata.trackNumber = *trackNumber;
        }
    }
    else if (atomType == "disk")
    {
        if (dataType != 0 && dataType != 21)
        {
            return;
        }
        const std::optional<std::uint16_t> discNumber = ParseMp4TrackDiskNumber(payload, payloadSize);
        if (metadata.discNumber == 0 && discNumber.has_value())
        {
            metadata.discNumber = *discNumber;
        }
    }
    else if (atomType == "covr")
    {
        if (payload == nullptr || payloadSize == 0 || !metadata.coverPath.empty())
        {
            return;
        }

        const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, payload, payloadSize);
        if (!coverPath.empty())
        {
            metadata.coverPath = coverPath;
        }
    }
}

void ReadMp4ItemAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    if (atomType == "covr" && !metadata.coverPath.empty())
    {
        return;
    }

    const std::size_t maxPayloadSize = Mp4MetadataPayloadLimit(atomType);
    (void)ForEachMp4ChildAtom(context.input, offset, limit, false, visitedAtoms, [&](const Mp4AtomHeader &atom)
                              {
        if (atom.atomType != "data")
        {
            return true;
        }

        const std::vector<uint8_t> data = ReadMp4AtomPayload(context.input, atom, maxPayloadSize);
        if (data.size() >= 8)
        {
            const uint32_t dataType = ReadBE32(data.data());
            ReadMp4DataAtom(context, metadata, atomType, dataType, data.data() + 8, data.size() - 8);
        }
        return true; });
}

void AppendPlainLyrics(RawLyrics &lyrics, std::string text)
{
    text = TrimText(std::move(text));
    if (!text.empty())
    {
        lyrics.text = std::move(text);
    }
}

void ReadMp4LyricsItem(ReadContext &context, RawLyrics &lyrics, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    (void)ForEachMp4ChildAtom(context.input, offset, limit, false, visitedAtoms, [&](const Mp4AtomHeader &atom)
                              {
        if (atom.atomType == "data")
        {
            const std::vector<uint8_t> data = ReadMp4AtomPayload(context.input, atom, kMaxLyricsBytes);
            if (data.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(data.data());
                if (AtomTypeIs(atomType, std::string_view(kMp4LyricsAtom.data(), kMp4LyricsAtom.size())))
                {
                    const DecodedField field = DecodeMp4TextData(dataType, data.data() + 8, data.size() - 8);
                    if (field.success && !field.value.empty())
                    {
                        AppendPlainLyrics(lyrics, field.value);
                    }
                }
            }
        }
        return true; });
}

void ReadMp4FreeformLyricsItem(ReadContext &context, RawLyrics &lyrics, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    std::string mean;
    std::string name;
    std::string text;

    const ParseStatus status = ForEachMp4ChildAtom(context.input, offset, limit, false, visitedAtoms, [&](const Mp4AtomHeader &atom)
                                                   {
        const std::size_t maxPayloadSize = atom.atomType == "data" ? kMaxLyricsBytes : kMaxTextFieldBytes;
        const std::vector<uint8_t> payload = ReadMp4AtomPayload(context.input, atom, maxPayloadSize);
        if (atom.atomType == "mean" || atom.atomType == "name")
        {
            if (payload.size() >= 4)
            {
                const DecodedField field = DecodeRawText(std::string_view(reinterpret_cast<const char *>(payload.data() + 4), payload.size() - 4));
                if (field.success)
                {
                    if (atom.atomType == "mean")
                    {
                        mean = field.value;
                    }
                    else
                    {
                        name = field.value;
                    }
                }
            }
        }
        else if (atom.atomType == "data")
        {
            if (payload.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(payload.data());
                const DecodedField field = DecodeMp4TextData(dataType, payload.data() + 8, payload.size() - 8);

                if (field.success && !field.value.empty())
                {
                    text = std::move(field.value);
                }
            }
        }

        return true; });

    if (status == ParseStatus::Malformed || status == ParseStatus::ResourceLimit)
    {
        return;
    }

    if (mean == "com.apple.iTunes" && ToLower(name) == "lyrics" && !text.empty())
    {
        ReadLyricsFromPlainText(lyrics, text);
    }
}
}

namespace tagreader_mp4
{
void ReadMp4Metadata(ReadContext &context, RawMetadata &metadata)
{
    TAGREADER_PROFILE_FUNCTION();
    
    if (!context.input.is_open())
    {
        return;
    }

    std::size_t visitedAtoms = 0;
    Mp4ItemCallbacks callbacks{};
    callbacks.onMetadataItem = [&](const Mp4AtomHeader &atom)
    {
        ReadMp4ItemAtom(context, metadata, atom.atomType, atom.payloadOffset, atom.atomEnd, visitedAtoms);
    };
    (void)WalkMp4IlstItems(context.input, 0, context.fileSize, visitedAtoms, callbacks);
}

void ReadMp4Lyrics(ReadContext &context, RawLyrics &lyrics)
{
    if (!context.input.is_open())
    {
        return;
    }

    std::size_t visitedAtoms = 0;
    Mp4ItemCallbacks callbacks{};
    callbacks.onLyricsItem = [&](const Mp4AtomHeader &atom)
    {
        ReadMp4LyricsItem(context, lyrics, atom.atomType, atom.payloadOffset, atom.atomEnd, visitedAtoms);
    };
    callbacks.onFreeformLyricsItem = [&](const Mp4AtomHeader &atom)
    {
        if (lyrics.text.empty() && lyrics.timedLines.empty())
        {
            ReadMp4FreeformLyricsItem(context, lyrics, atom.payloadOffset, atom.atomEnd, visitedAtoms);
        }
    };
    (void)WalkMp4IlstItems(context.input, 0, context.fileSize, visitedAtoms, callbacks);
}
}
