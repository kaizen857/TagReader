#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Exhaustive typed cover-processing failure contract.
//
// Absence of embedded or sidecar cover art is "no-art" (an empty coverPath
// result), never a CoverProcessingError.
enum class CoverErrorCode
{
    ExportDirectoryUnavailable,  // cover export directory cannot be created/validated/hardened
    SidecarDiscoveryFailed,      // sidecar candidate discovery failed (unreadable directory etc.)
    SidecarEntryLimitExceeded,   // more than CoverProcessingOptions::maxSidecarEntries candidates
    SourceReadFailed,            // encoded source cover bytes could not be read
    SourceBudgetExceeded,        // cumulative encoded source cover bytes exceed maxSourceCoverBytes
    DecodeFailed,                // source cover data could not be decoded
    CacheReadFailed,             // existing cover cache entry could not be read/validated
    CacheWriteFailed,            // cover cache entry could not be written
    PublicationFailed,           // final cover file could not be published atomically
};

// Typed cover failure carrying the CoverErrorCode and an optional offending
// path. what() reports both the code and the path (when present).
class CoverProcessingError : public std::runtime_error
{
public:
    CoverProcessingError(CoverErrorCode code, std::string message)
        : CoverProcessingError(code, std::move(message), std::filesystem::path{})
    {
    }

    CoverProcessingError(CoverErrorCode code, std::string message, std::filesystem::path path)
        : std::runtime_error(BuildMessage(code, message, path)),
          code_(code)
    {
        if (!path.empty())
        {
            path_ = std::move(path);
        }
    }

    CoverErrorCode code() const noexcept
    {
        return code_;
    }

    const std::optional<std::filesystem::path> &path() const noexcept
    {
        return path_;
    }

private:
    static const char *CodeName(CoverErrorCode code) noexcept
    {
        switch (code)
        {
        case CoverErrorCode::ExportDirectoryUnavailable:
            return "ExportDirectoryUnavailable";
        case CoverErrorCode::SidecarDiscoveryFailed:
            return "SidecarDiscoveryFailed";
        case CoverErrorCode::SidecarEntryLimitExceeded:
            return "SidecarEntryLimitExceeded";
        case CoverErrorCode::SourceReadFailed:
            return "SourceReadFailed";
        case CoverErrorCode::SourceBudgetExceeded:
            return "SourceBudgetExceeded";
        case CoverErrorCode::DecodeFailed:
            return "DecodeFailed";
        case CoverErrorCode::CacheReadFailed:
            return "CacheReadFailed";
        case CoverErrorCode::CacheWriteFailed:
            return "CacheWriteFailed";
        case CoverErrorCode::PublicationFailed:
            return "PublicationFailed";
        }
        return "Unknown";
    }

    static std::string BuildMessage(CoverErrorCode code, const std::string &message, const std::filesystem::path &path)
    {
        std::string out = "cover processing error [" + std::string(CodeName(code)) + "]: " + message;
        if (!path.empty())
        {
            out += " (path: " + path.string() + ")";
        }
        return out;
    }
    CoverErrorCode code_;
    std::optional<std::filesystem::path> path_;
};

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
        Balanced = 3,
        Best = 6
    } pngCompression{PngCompressionLevel::Fast};

    // Which cover outputs a single Read (or each referenced audio read in a
    // ReadCueSheet) must produce. FullAndThumbnail is the default and preserves
    // the historical output set (full PNG plus thumbnail PNG).
    enum class CoverProcessingMode
    {
        Disabled,         // no cover output at all; metadata/lyrics unaffected
        ThumbnailOnly,    // only the thumbnail PNG
        FullOnly,         // only the full-size PNG
        FullAndThumbnail, // both (historical default)
    } mode{CoverProcessingMode::FullAndThumbnail};

    // What to do when a cover-specific failure (CoverProcessingError) occurs.
    // Propagate is the default and preserves historical behavior (the failure
    // reaches the caller unchanged, with the same code() and path()).
    enum class CoverFailurePolicy
    {
        Propagate, // rethrow the CoverProcessingError unchanged
        Ignore,    // swallow the cover failure, clear artwork, continue with
                   // metadata/lyrics as if the track had no cover
    } failurePolicy{CoverFailurePolicy::Propagate};

    // Upper bound on sidecar cover candidates examined during discovery for one
    // read. Exceeding it is CoverErrorCode::SidecarEntryLimitExceeded.
    std::size_t maxSidecarEntries{4096};

    // Cumulative budget, in bytes, of encoded source cover data examined in a
    // single Read (or per referenced audio file in a ReadCueSheet). Embedded
    // cover bytes and any subsequent sidecar fallback share this one budget;
    // generated full/thumbnail PNG bytes and already-cached cover file bytes do
    // NOT consume it. A value of 0 disables source-art reads entirely while
    // metadata/lyrics reading continues normally.
    std::uint64_t maxSourceCoverBytes{64 * 1024 * 1024};
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
