#include "formats/cue/CueReader.hpp"
#include "formats/cue/CuePathResolver.hpp"
#include "formats/cue/CueParser.hpp"
#include "formats/cue/CueTiming.hpp"
#include "formats/cue/CueTextLoader.hpp"

#include "core/TagPipeline.hpp"
#include "cover/CoverCache.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace
{
constexpr std::array<std::string_view, 5> kCueCoverNames{
    "cover",
    "front",
    "folder",
    "album",
    "artwork",
};

constexpr std::array<std::string_view, 7> kCueCoverExtensions{
    ".png",
    ".jpg",
    ".jpeg",
    ".bmp",
    ".webp",
    ".gif",
    ".tiff",
};

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

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index])))
        {
            return false;
        }
    }

    return true;
}

bool IsCueCoverFile(const std::filesystem::path &path)
{
    const std::string extension = path.extension().string();
    for (const std::string_view allowedExtension : kCueCoverExtensions)
    {
        if (EqualsIgnoreCase(extension, allowedExtension))
        {
            return true;
        }
    }

    return false;
}

std::filesystem::path ResolveCueCoverExportDir(const std::filesystem::path &coverExportDir)
{
    if (!coverExportDir.empty())
    {
        tagreader_core::ValidateCoverExportDir(coverExportDir);
        return coverExportDir;
    }

    const std::filesystem::path defaultCoverExportDir = tagreader_core::DefaultCoverExportDir();
    tagreader_core::ValidateDefaultCoverExportDir(defaultCoverExportDir);
    return defaultCoverExportDir;
}

std::optional<std::filesystem::path> ExportCueSidecarCover(const std::filesystem::path &audioPath, const std::filesystem::path &coverExportDir)
{
    const std::filesystem::path resolvedCoverExportDir = ResolveCueCoverExportDir(coverExportDir);
    const std::filesystem::path audioDirectory = audioPath.parent_path();
    if (audioDirectory.empty())
    {
        return std::nullopt;
    }

    std::error_code ec;
    std::array<std::vector<std::filesystem::path>, kCueCoverNames.size()> candidatesPerPriority{};
    std::size_t candidateCount = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(audioDirectory, ec))
    {
        if (ec || candidateCount >= 256)
        {
            break;
        }

        std::error_code entryEc;
        const std::filesystem::file_status status = entry.symlink_status(entryEc);
        if (entryEc || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) || !IsCueCoverFile(entry.path()))
        {
            continue;
        }

        const std::string stem = entry.path().stem().string();
        for (std::size_t priority = 0; priority < kCueCoverNames.size(); ++priority)
        {
            if (EqualsIgnoreCase(stem, kCueCoverNames[priority]))
            {
                candidatesPerPriority[priority].push_back(entry.path());
                ++candidateCount;
                break;
            }
        }
    }

    for (auto &candidates : candidatesPerPriority)
    {
        std::sort(candidates.begin(), candidates.end(), [](const std::filesystem::path &left, const std::filesystem::path &right)
        {
            return left.filename().string() < right.filename().string();
        });
    }

    for (const auto &candidates : candidatesPerPriority)
    {
        for (const std::filesystem::path &candidatePath : candidates)
        {
            std::error_code sizeEc;
            const std::uintmax_t fileSize = std::filesystem::file_size(candidatePath, sizeEc);
            if (sizeEc || fileSize == 0 || fileSize > 64ULL * 1024ULL * 1024ULL)
            {
                continue;
            }

            std::ifstream input(candidatePath, std::ios::binary);
            if (!input)
            {
                continue;
            }

            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
            if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            {
                continue;
            }

            const std::filesystem::path coverPath = tagreader_cover::WriteCoverAsPng(resolvedCoverExportDir, bytes.data(), bytes.size());
            if (!coverPath.empty())
            {
                return coverPath;
            }
        }
    }

    return std::nullopt;
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

        MusicTag audioTag = tagreader_core::ReadTag(resolution.resolvedPath, coverExportDir);
        if (audioTag.coverPath().empty())
        {
            const std::optional<std::filesystem::path> sidecarCoverPath = ExportCueSidecarCover(resolution.resolvedPath, coverExportDir);
            if (sidecarCoverPath.has_value())
            {
                audioTag.setCoverPath(*sidecarCoverPath);
            }
        }
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
