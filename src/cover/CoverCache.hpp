#ifndef TAGREADER_COVER_COVERCACHE_HPP
#define TAGREADER_COVER_COVERCACHE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

struct CoverProcessingOptions;

namespace tagreader_core
{
struct ReadContext;
}

namespace tagreader_cover
{
std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size);

struct CoverPaths
{
    std::filesystem::path fullSizePath;
    std::filesystem::path thumbnailPath;
};

CoverPaths WriteCoverWithThumbnail(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size, const CoverProcessingOptions &options);

CoverPaths ExportCoverFromContext(const tagreader_core::ReadContext &context, const uint8_t *data, std::size_t size);
}

#endif
