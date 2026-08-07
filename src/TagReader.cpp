#include "TagReader.hpp"
#include "core/TagPipeline.hpp"
#include "cover/SidecarCover.hpp"
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

MusicTag TagReader::ExportFolderCover(std::string folderPath, std::string coverExportDir, const CoverProcessingOptions &options)
{
    tagreader_core::ReadContext context;
    context.filePath = folderPath;
    context.coverExportDir = coverExportDir;
    context.coverOptions = &options;

    try
    {
        const tagreader_cover::CoverPaths paths = tagreader_cover::ExportSidecarCoverFromDirectory(folderPath, context);
        MusicTag tag;
        if (!paths.fullSizePath.empty())
        {
            tag.setCoverPath(paths.fullSizePath);
        }
        if (!paths.thumbnailPath.empty())
        {
            tag.setThumbnailPath(paths.thumbnailPath);
        }
        return tag;
    }
    catch (const CoverProcessingError &)
    {
        // 本 API 契约：无候选或全部失败统一返回空 thumbnailPath，永不抛。
        return MusicTag{};
    }
}
