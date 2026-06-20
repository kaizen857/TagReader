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

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath)
{
    (void)filePath;
    return {};
}

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    (void)filePath;
    (void)coverExportDir;
    return {};
}
