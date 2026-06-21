#include "cover/SidecarCover.hpp"

#include "cover/CoverCache.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
    const std::string extension = path.extension().string();
    for (const std::string_view allowedExtension : kSidecarCoverExtensions)
    {
        if (EqualsIgnoreCase(extension, allowedExtension))
        {
            return true;
        }
    }

    return false;
}
}

namespace tagreader_cover
{
std::optional<std::filesystem::path> ExportSidecarCover(const std::filesystem::path &audioPath, const std::filesystem::path &coverExportDir)
{
    if (audioPath.empty() || coverExportDir.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path audioDirectory = audioPath.parent_path();
    if (audioDirectory.empty())
    {
        return std::nullopt;
    }

    std::error_code ec;
    std::array<std::vector<std::filesystem::path>, kSidecarCoverNames.size()> candidatesPerPriority{};
    std::size_t candidateCount = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(audioDirectory, ec))
    {
        if (ec || candidateCount >= 256)
        {
            break;
        }

        std::error_code entryEc;
        const std::filesystem::file_status status = entry.symlink_status(entryEc);
        if (entryEc || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) || !IsSidecarCoverFile(entry.path()))
        {
            continue;
        }

        const std::string stem = entry.path().stem().string();
        for (std::size_t priority = 0; priority < kSidecarCoverNames.size(); ++priority)
        {
            if (EqualsIgnoreCase(stem, kSidecarCoverNames[priority]))
            {
                candidatesPerPriority[priority].push_back(entry.path());
                ++candidateCount;
                break;
            }
        }
    }

    for (auto &candidates : candidatesPerPriority)
    {
        std::sort(candidates.begin(), candidates.end(), [](const std::filesystem::path &left, const std::filesystem::path &right)
        {
            return left.filename().string() < right.filename().string();
        });
    }

    for (const auto &candidates : candidatesPerPriority)
    {
        for (const std::filesystem::path &candidatePath : candidates)
        {
            std::error_code sizeEc;
            const std::uintmax_t fileSize = std::filesystem::file_size(candidatePath, sizeEc);
            if (sizeEc || fileSize == 0 || fileSize > 64ULL * 1024ULL * 1024ULL)
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

            const std::filesystem::path coverPath = WriteCoverAsPng(coverExportDir, bytes.data(), bytes.size());
            if (!coverPath.empty())
            {
                return coverPath;
            }
        }
    }

    return std::nullopt;
}
}
