#include "cover/SidecarCover.hpp"

#include "TagReader.hpp"
#include "core/ReadContext.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
constexpr std::array<std::string_view, 5> kSidecarCoverNames{
    "cover",
    "front",
    "folder",
    "album",
    "artwork",
};

constexpr std::array<std::string_view, 7> kSidecarCoverExtensions{
    ".png",
    ".jpg",
    ".jpeg",
    ".bmp",
    ".webp",
    ".gif",
    ".tiff",
};

constexpr std::size_t kDefaultMaxSidecarEntries = 4096;
constexpr std::uint64_t kLegacySingleCandidateLimit = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::string Utf8Text(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index])))
        {
            return false;
        }
    }

    return true;
}

bool IsSidecarCoverFile(const std::filesystem::path &path)
{
    const std::string extension = Utf8Text(path.extension());
    for (const std::string_view allowedExtension : kSidecarCoverExtensions)
    {
        if (EqualsIgnoreCase(extension, allowedExtension))
        {
            return true;
        }
    }

    return false;
}

[[noreturn]] void ThrowSidecarCoverError(CoverErrorCode code, const std::string &message, const std::filesystem::path &path = {})
{
    throw CoverProcessingError{code, message, path};
}
}

namespace tagreader_cover
{
CoverPaths ExportSidecarCoverFromDirectory(const std::filesystem::path &directory, tagreader_core::ReadContext &context)
{
    if (directory.empty())
    {
        return {};
    }

    const CoverProcessingOptions *options = context.coverOptions;
    if (options != nullptr && options->maxSourceCoverBytes == 0)
    {
        return {};
    }

    const std::size_t maxSidecarEntries = (options != nullptr) ? options->maxSidecarEntries : kDefaultMaxSidecarEntries;

    std::error_code ec;
    std::array<std::vector<std::filesystem::path>, kSidecarCoverNames.size()> candidatesPerPriority{};
    std::size_t candidateCount = 0;

    std::filesystem::directory_iterator it(directory, ec);
    const std::filesystem::directory_iterator end;
    if (!ec)
    {
        for (; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::directory_entry &entry = *it;
            std::error_code entryEc;
            const std::filesystem::file_status status = entry.symlink_status(entryEc);
            if (entryEc || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) || !IsSidecarCoverFile(entry.path()))
            {
                continue;
            }

            const std::string stem = Utf8Text(entry.path().stem());
            for (std::size_t priority = 0; priority < kSidecarCoverNames.size(); ++priority)
            {
                if (EqualsIgnoreCase(stem, kSidecarCoverNames[priority]))
                {
                    if (candidateCount >= maxSidecarEntries)
                    {
                        ThrowSidecarCoverError(CoverErrorCode::SidecarEntryLimitExceeded,
                                               "sidecar cover candidates exceed maxSidecarEntries",
                                               directory);
                    }
                    candidatesPerPriority[priority].push_back(entry.path());
                    ++candidateCount;
                    break;
                }
            }
        }
    }
    if (ec)
    {
        ThrowSidecarCoverError(CoverErrorCode::SidecarDiscoveryFailed,
                               "sidecar cover discovery failed: " + ec.message(),
                               directory);
    }

    for (auto &candidates : candidatesPerPriority)
    {
        std::sort(candidates.begin(), candidates.end(), [](const std::filesystem::path &left, const std::filesystem::path &right)
        {
            return Utf8Text(left.filename()) < Utf8Text(right.filename());
        });
    }

    const std::uint64_t singleCandidateLimit = (options != nullptr) ? options->maxSourceCoverBytes : kLegacySingleCandidateLimit;

    for (const auto &candidates : candidatesPerPriority)
    {
        for (const std::filesystem::path &candidatePath : candidates)
        {
            std::error_code sizeEc;
            const std::uintmax_t fileSize = std::filesystem::file_size(candidatePath, sizeEc);
            if (sizeEc || fileSize == 0 || fileSize > singleCandidateLimit)
            {
                continue;
            }

            std::ifstream input(candidatePath, std::ios::binary);
            if (!input)
            {
                continue;
            }

            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
            if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            {
                continue;
            }

            const CoverPaths paths = ExportCoverFromContext(context, bytes.data(), bytes.size());
            if (!paths.fullSizePath.empty() || !paths.thumbnailPath.empty())
            {
                return paths;
            }
        }
    }

    return {};
}

CoverPaths ExportSidecarCover(tagreader_core::ReadContext &context)
{
    if (context.filePath.empty() || context.coverExportDir.empty())
    {
        return {};
    }

    const std::filesystem::path audioDirectory = context.filePath.parent_path();
    if (audioDirectory.empty())
    {
        return {};
    }

    return ExportSidecarCoverFromDirectory(audioDirectory, context);
}
}
