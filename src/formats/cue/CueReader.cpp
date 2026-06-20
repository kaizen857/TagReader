#include "formats/cue/CueReader.hpp"
#include "formats/cue/CuePathResolver.hpp"
#include "formats/cue/CueParser.hpp"
#include "formats/cue/CueTiming.hpp"
#include "formats/cue/CueTextLoader.hpp"

#include "core/TagPipeline.hpp"

#include <cctype>
#include <iterator>
#include <optional>
#include <string_view>

namespace
{
std::string TrimAscii(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::optional<std::uint16_t> ParseCueUnsigned16(std::string_view text)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const char ch : trimmed)
    {
        if (ch < '0' || ch > '9')
        {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
        if (value > std::numeric_limits<std::uint16_t>::max())
        {
            return std::nullopt;
        }
    }

    return static_cast<std::uint16_t>(value);
}

void ApplyCueMetadata(const tagreader_cue::CueGlobal &global, const tagreader_cue::CueFile &file, const tagreader_cue::CueTrack &track, MusicTag &tag)
{
    if (!global.title.empty())
    {
        tag.setAlbum(global.title);
    }
    if (!global.performer.empty())
    {
        tag.setAlbumArtist(global.performer);
    }
    if (!global.songwriter.empty())
    {
        tag.setComposer(global.songwriter);
    }
    if (!global.genre.empty())
    {
        tag.setGenre(global.genre);
    }
    if (!global.year.empty())
    {
        const std::optional<std::uint16_t> year = ParseCueUnsigned16(global.year);
        if (year.has_value())
        {
            tag.setYear(*year);
        }
    }
    if (!global.discNumber.empty())
    {
        const std::optional<std::uint16_t> discNumber = ParseCueUnsigned16(global.discNumber);
        if (discNumber.has_value())
        {
            tag.setDiscNumber(*discNumber);
        }
    }

    if (!file.title.empty())
    {
        tag.setAlbum(file.title);
    }
    if (!file.performer.empty())
    {
        tag.setAlbumArtist(file.performer);
    }
    if (!file.songwriter.empty())
    {
        tag.setComposer(file.songwriter);
    }

    if (!track.title.empty())
    {
        tag.setTitle(track.title);
    }
    if (!track.performer.empty())
    {
        tag.setArtist(track.performer);
    }
    if (!track.songwriter.empty())
    {
        tag.setComposer(track.songwriter);
    }
    if (track.number != 0)
    {
        tag.setTrackNumber(track.number);
    }
}

std::vector<MusicTag> BuildCueTags(const std::filesystem::path &cuePath, const tagreader_cue::ParsedCueSheet &parsedSheet, const std::filesystem::path &coverExportDir)
{
    std::vector<MusicTag> tags;
    for (const auto &file : parsedSheet.files)
    {
        const tagreader_cue::CuePathResolution resolution = tagreader_cue::ResolveCueFileReference(cuePath, file.name);
        if (resolution.status != tagreader_cue::CuePathResolutionStatus::Resolved)
        {
            continue;
        }

        const MusicTag audioTag = tagreader_core::ReadTag(resolution.resolvedPath, coverExportDir);
        std::vector<MusicTag> fileTags;
        fileTags.reserve(file.tracks.size());
        for (const auto &track : file.tracks)
        {
            MusicTag cueTag = audioTag;
            ApplyCueMetadata(parsedSheet.global, file, track, cueTag);
            cueTag.setFilePath(resolution.resolvedPath);
            fileTags.push_back(std::move(cueTag));
        }
        if (!ApplyCueTiming(file, fileTags))
        {
            return {};
        }
        tags.insert(tags.end(), std::make_move_iterator(fileTags.begin()), std::make_move_iterator(fileTags.end()));
    }

    return tags;
}
}

namespace tagreader_cue
{
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath)
{
    const std::optional<std::string> cueText = LoadCueTextUtf8(cuePath);
    if (!cueText.has_value())
    {
        return {};
    }

    const std::optional<ParsedCueSheet> parsedSheet = ParseCueSheet(*cueText);
    if (!parsedSheet.has_value())
    {
        return {};
    }

    for (const auto &file : parsedSheet->files)
    {
        if (ResolveCueFileReference(cuePath, file.name).status != CuePathResolutionStatus::Resolved)
        {
            return {};
        }
    }

    return BuildCueTags(cuePath, *parsedSheet, {});
}

std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir)
{
    const std::optional<std::string> cueText = LoadCueTextUtf8(cuePath);
    if (!cueText.has_value())
    {
        return {};
    }

    const std::optional<ParsedCueSheet> parsedSheet = ParseCueSheet(*cueText);
    if (!parsedSheet.has_value())
    {
        return {};
    }

    for (const auto &file : parsedSheet->files)
    {
        if (ResolveCueFileReference(cuePath, file.name).status != CuePathResolutionStatus::Resolved)
        {
            return {};
        }
    }

    return BuildCueTags(cuePath, *parsedSheet, coverExportDir);
}
}
