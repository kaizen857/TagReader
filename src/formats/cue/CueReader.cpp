#include "formats/cue/CueReader.hpp"

namespace tagreader_cue
{
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath)
{
    (void)cuePath;
    return {};
}

std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir)
{
    (void)cuePath;
    (void)coverExportDir;
    return {};
}
}
