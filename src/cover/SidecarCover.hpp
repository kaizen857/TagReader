#ifndef TAGREADER_COVER_SIDECARCOVER_HPP
#define TAGREADER_COVER_SIDECARCOVER_HPP

#include <filesystem>
#include <optional>

namespace tagreader_cover
{
std::optional<std::filesystem::path> ExportSidecarCover(const std::filesystem::path &audioPath, const std::filesystem::path &coverExportDir);
}

#endif
