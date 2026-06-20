#include "TagReader.hpp"
#include "core/TagPipeline.hpp"
#include "formats/cue/CueReader.hpp"

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
    return tagreader_cue::ReadCueSheet(filePath);
}

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    return tagreader_cue::ReadCueSheet(filePath, coverExportDir);
}
