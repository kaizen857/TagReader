#include "TagReader.hpp"
#include "core/TagPipeline.hpp"

MusicTag TagReader::Read(const std::filesystem::path &filePath)
{
    return tagreader_core::ReadTag(filePath, {});
}

MusicTag TagReader::Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    return tagreader_core::ReadTag(filePath, coverExportDir);
}
