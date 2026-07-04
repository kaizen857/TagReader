#ifndef TAGREADER_FORMATS_CUE_CUEREADER_HPP
#define TAGREADER_FORMATS_CUE_CUEREADER_HPP

#include "Tag.hpp"

#include <filesystem>
#include <vector>

struct CoverProcessingOptions;

namespace tagreader_cue
{
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath);
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir);
std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &cuePath, const std::filesystem::path &coverExportDir, const CoverProcessingOptions &options);
}

#endif
