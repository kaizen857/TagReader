#include "formats/cue/CueParser.hpp"
#include "formats/cue/CueReader.hpp"
#include "formats/cue/CueTextLoader.hpp"

namespace tagreader_cue
{
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath)
{
    const std::optional<std::string> cueText = LoadCueTextUtf8(cuePath);
    if (!cueText.has_value())
    {
        return {};
    }

    if (!ParseCueSheet(*cueText).has_value())
    {
        return {};
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

    if (!ParseCueSheet(*cueText).has_value())
    {
        return {};
    }

    return {};
}
}
