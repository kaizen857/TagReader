#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <cstdint>
#include <filesystem>
#include <vector>

struct CoverProcessingOptions
{
    bool generateThumbnail{true};

    struct ThumbnailSize
    {
        uint32_t width{256};
        uint32_t height{256};
        bool maintainAspectRatio{true};
    } thumbnailSize;

    enum class ScalingQuality
    {
        Fast,
        Good,
        Best
    } scalingQuality{ScalingQuality::Fast};

    enum class PngCompressionLevel
    {
        Fast = 1,
        Balanced = 6,
        Best = 9
    } pngCompression{PngCompressionLevel::Fast};
};

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);
    static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
    static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir, const CoverProcessingOptions &options);
    
    static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath);
    static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
    static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir, const CoverProcessingOptions &options);
};

#endif
