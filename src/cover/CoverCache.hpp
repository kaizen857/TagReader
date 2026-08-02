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
// Debits `size` encoded source-cover bytes against the per-read budget shared
// by embedded covers and sidecar fallback. Returns false when the source-art
// read must be skipped without error (zero budget); throws
// CoverErrorCode::SourceBudgetExceeded when the cumulative budget would be
// exceeded. Directory entry names/metadata never debit; only bytes of
// candidates actually opened and read are passed here.
bool DebitCoverSourceBudget(tagreader_core::ReadContext &context, std::size_t size);

std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size);

struct CoverPaths
{
    std::filesystem::path fullSizePath;
    std::filesystem::path thumbnailPath;
};

CoverPaths WriteCoverWithThumbnail(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size, const CoverProcessingOptions &options);

CoverPaths ExportCoverFromContext(tagreader_core::ReadContext &context, const uint8_t *data, std::size_t size);
}

#endif
