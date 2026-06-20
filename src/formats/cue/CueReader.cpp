#include "formats/cue/CueReader.hpp"
#include "formats/cue/CuePathResolver.hpp"
#include "formats/cue/CueParser.hpp"
#include "formats/cue/CueTextLoader.hpp"

#include <optional>

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

    return {};
}

std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir)
{
    (void)coverExportDir;
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

    return {};
}
}
