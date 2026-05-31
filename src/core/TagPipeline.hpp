#ifndef TAGREADER_CORE_TAGPIPELINE_HPP
#define TAGREADER_CORE_TAGPIPELINE_HPP

#include "Tag.hpp"
#include "core/ReadContext.hpp"
#include "core/TagFormat.hpp"

#include <filesystem>

namespace tagreader_core
{
MusicTag ReadTag(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
}

#endif
