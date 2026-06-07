#ifndef TAGREADER_CORE_READCONTEXT_HPP
#define TAGREADER_CORE_READCONTEXT_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <memory>
#include <string>

struct AVFormatContext;

namespace tagreader_core
{
enum class DetectedContainer
{
    Unknown,
    Mp3,
    Flac,
    OggVorbis,
    Mp4,
    Ape,
};

struct ReadContext
{
    struct FormatContextDeleter
    {
        void operator()(AVFormatContext *context) const noexcept;
    };

    std::filesystem::path filePath;
    std::filesystem::path coverExportDir;
    std::ifstream input;
    std::uintmax_t fileSize{};
    std::filesystem::file_time_type lastModified{};
    std::unique_ptr<AVFormatContext, FormatContextDeleter> formatContext;
    int audioStreamIndex{-1};
    DetectedContainer detectedContainer{DetectedContainer::Unknown};
    std::string containerName;
    // Optional debug output channel for parser errors.
    // nullptr means no diagnostics are written.
    std::ostream *diagnostics = nullptr;
};
}

#endif
