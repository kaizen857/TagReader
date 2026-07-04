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

MusicTag TagReader::Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir, const CoverProcessingOptions &options)
{
    return tagreader_core::ReadTag(filePath, coverExportDir, options);
}

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath)
{
    return tagreader_cue::ReadCueSheet(filePath);
}

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    return tagreader_cue::ReadCueSheet(filePath, coverExportDir);
}

std::vector<MusicTag> TagReader::ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir, const CoverProcessingOptions &options)
{
    return tagreader_cue::ReadCueSheet(filePath, coverExportDir, options);
}
