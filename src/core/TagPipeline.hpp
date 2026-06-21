#ifndef TAGREADER_CORE_TAGPIPELINE_HPP
#define TAGREADER_CORE_TAGPIPELINE_HPP

#include "Tag.hpp"
#include "core/ReadContext.hpp"
#include "core/TagFormat.hpp"

#include <filesystem>
#include <string_view>

namespace tagreader_core
{
std::filesystem::path DefaultCoverExportDir();
void ValidateCoverExportDir(const std::filesystem::path &coverExportDir);
void ValidateDefaultCoverExportDir(const std::filesystem::path &coverExportDir);
bool IsCoverExportOrCacheError(std::string_view message);
MusicTag ReadTag(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
}

#endif
