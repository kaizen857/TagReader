#include "TagReader.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/log.h>
#ifdef __cplusplus
}
#endif

#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
constexpr int kSkipReturnCode = 77;

void PrintUsage(std::string_view program)
{
    std::cerr << "usage: " << program << " <cover-export-dir> <audio-file-path | sample-dir> [audio-file-path ...]\n";
}

void PrintTagSummary(const std::filesystem::path &samplePath, const MusicTag &tag)
{
    std::cout << "sample: " << samplePath.string() << '\n';
    std::cout << "title: " << tag.title() << '\n';
    std::cout << "lyricsCount: " << tag.lyrics().size() << '\n';
    std::cout << "coverPath: " << tag.coverPath().string() << '\n';
}

bool ErrorMentionsCoverCacheAndPath(const std::exception &ex, const std::filesystem::path &coverPath)
{
    const std::string message = ex.what();
    return message.find("cover cache") != std::string::npos && message.find(coverPath.string()) != std::string::npos;
}

bool RunPollutedCoverCacheSmoke(const std::filesystem::path &samplePath, const std::filesystem::path &coverExportDir, const std::filesystem::path &coverPath)
{
    std::ofstream output(coverPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        std::cerr << "cover cache pollution setup failed for " << coverPath.string() << '\n';
        return false;
    }
    output << "polluted cover cache entry\n";
    output.close();
    if (!output)
    {
        std::cerr << "cover cache pollution write failed for " << coverPath.string() << '\n';
        return false;
    }

    MusicTag tag;
    try
    {
        tag = TagReader::Read(samplePath, coverExportDir);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "polluted cache should be auto-fixed, not throw for " << coverPath.string() << ": " << ex.what() << '\n';
        return false;
    }

    if (tag.coverPath().empty() || tag.coverPath() != coverPath)
    {
        std::cerr << "polluted cache auto-fix failed for " << coverPath.string() << '\n';
        return false;
    }

    std::cout << "cover cache polluted auto-fix passed: " << coverPath.string() << '\n';
    return true;
}

std::filesystem::path SampleCoverExportDir(const std::filesystem::path &coverExportDir, const std::filesystem::path &samplePath)
{
    return coverExportDir / samplePath.filename();
}

std::vector<std::filesystem::path> LoadSmokeSamplesFromManifest(const std::filesystem::path &sampleDir)
{
    const std::filesystem::path manifestPath = sampleDir / "MANIFEST.txt";
    std::ifstream manifest(manifestPath);
    if (!manifest)
    {
        throw std::runtime_error("missing security sample manifest: " + manifestPath.string());
    }

    enum class Section
    {
        None,
        SmokeSamples,
        Other,
    };

    Section currentSection = Section::None;
    std::vector<std::filesystem::path> samples;
    for (std::string line; std::getline(manifest, line);)
    {
        if (line == "## smoke_samples")
        {
            currentSection = Section::SmokeSamples;
            continue;
        }
        if (!line.empty() && line.starts_with("## "))
        {
            currentSection = Section::Other;
            continue;
        }
        if (line.empty() || line.starts_with("#"))
        {
            continue;
        }
        if (currentSection == Section::SmokeSamples)
        {
            samples.emplace_back(line);
        }
    }

    return samples;
}

bool RunCoverCacheSmoke(const std::filesystem::path &samplePath, const std::filesystem::path &coverExportDir, const MusicTag &firstTag)
{
    if (firstTag.coverPath().empty())
    {
        return true;
    }

    const std::filesystem::path coverPath = firstTag.coverPath();
    std::error_code ec;
    const auto firstMtime = std::filesystem::last_write_time(coverPath, ec);
    if (ec)
    {
        std::cerr << "cover cache mtime error for " << coverPath.string() << ": " << ec.message() << '\n';
        return false;
    }

    const MusicTag repeatedTag = TagReader::Read(samplePath, coverExportDir);
    if (repeatedTag.coverPath() != coverPath)
    {
        std::cerr << "cover cache repeated path changed for " << samplePath.string() << '\n';
        return false;
    }
    const auto repeatedMtime = std::filesystem::last_write_time(coverPath, ec);
    if (ec || repeatedMtime != firstMtime)
    {
        std::cerr << "cover cache repeated read modified mtime for " << coverPath.string() << '\n';
        return false;
    }

    constexpr int kConcurrentReads = 8;
    std::vector<std::future<std::filesystem::path>> futures;
    futures.reserve(kConcurrentReads);
    for (int worker = 0; worker < kConcurrentReads; ++worker)
    {
        futures.push_back(std::async(std::launch::async, [samplePath, coverExportDir]()
                                     { return TagReader::Read(samplePath, coverExportDir).coverPath(); }));
    }

    for (auto &future : futures)
    {
        try
        {
            if (future.get() != coverPath)
            {
                std::cerr << "cover cache concurrent path changed for " << samplePath.string() << '\n';
                return false;
            }
        }
        catch (const std::exception &ex)
        {
            std::cerr << "cover cache concurrent read error for " << samplePath.string() << ": " << ex.what() << '\n';
            return false;
        }
    }

    const auto concurrentMtime = std::filesystem::last_write_time(coverPath, ec);
    if (ec || concurrentMtime != firstMtime)
    {
        std::cerr << "cover cache concurrent read modified mtime for " << coverPath.string() << '\n';
        return false;
    }

    if (!RunPollutedCoverCacheSmoke(samplePath, coverExportDir, coverPath))
    {
        return false;
    }

    return true;
}

std::filesystem::path SampleCoverPath(const std::filesystem::path &coverExportDir, const std::filesystem::path &samplePath)
{
    return coverExportDir / samplePath.filename() / "4b" / "5c5c92cec3b23e6a294fc0eea43234ef5126c5a64f4c6c531ac8430ab0b844.png";
}
} // namespace

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_QUIET);

    if (argc < 3)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::filesystem::path coverExportDir = argv[1];
    (void)coverExportDir;
    std::vector<std::filesystem::path> samplePaths;
    if (argc == 3 && std::filesystem::is_directory(argv[2]))
    {
        samplePaths = LoadSmokeSamplesFromManifest(argv[2]);
        if (samplePaths.empty())
        {
            std::cout << "security smoke skipped: no smoke samples were generated under " << argv[2] << '\n';
            return kSkipReturnCode;
        }
    }
    else
    {
        for (int i = 2; i < argc; ++i)
        {
            samplePaths.emplace_back(argv[i]);
        }
    }

    bool hasFailure = false;
    for (const std::filesystem::path &samplePath : samplePaths)
    {
        const std::filesystem::path sampleCoverExportDir = SampleCoverExportDir(coverExportDir, samplePath);
        std::error_code ec;
        std::filesystem::remove_all(sampleCoverExportDir, ec);
        if (ec)
        {
            hasFailure = true;
            std::cerr << "failed to reset cover export directory for " << samplePath.string() << ": " << ec.message() << '\n';
            continue;
        }
        std::filesystem::create_directories(sampleCoverExportDir, ec);
        if (ec)
        {
            hasFailure = true;
            std::cerr << "failed to prepare cover export directory for " << samplePath.string() << ": " << ec.message() << '\n';
            continue;
        }
        try
        {
            const MusicTag tag = TagReader::Read(samplePath, sampleCoverExportDir);
            PrintTagSummary(samplePath, tag);
            if (!RunCoverCacheSmoke(samplePath, sampleCoverExportDir, tag))
            {
                hasFailure = true;
            }
        }
        catch (const std::exception &ex)
        {
            hasFailure = true;
            std::cerr << "TagReader error for " << samplePath.string() << ": " << ex.what() << '\n';
        }
        catch (...)
        {
            hasFailure = true;
            std::cerr << "TagReader unknown error for " << samplePath.string() << '\n';
        }
    }

    return hasFailure ? 1 : 0;
}
