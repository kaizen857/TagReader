#ifndef TAGREADER_FORMATS_CUE_CUEREADER_HPP
#define TAGREADER_FORMATS_CUE_CUEREADER_HPP

#include "Tag.hpp"

#include <filesystem>
#include <vector>

namespace tagreader_cue
{
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath);
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir);
}

#endif
