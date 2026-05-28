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
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
void PrintUsage(std::string_view program)
{
    std::cerr << "usage: " << program << " <cover-export-dir> <audio-file-path> [audio-file-path ...]\n";
}

void PrintTagSummary(const std::filesystem::path &samplePath, const MusicTag &tag)
{
    std::cout << "sample: " << samplePath.string() << '\n';
    std::cout << "title: " << tag.title() << '\n';
    std::cout << "lyricsCount: " << tag.lyrics().size() << '\n';
    std::cout << "coverPath: " << tag.coverPath().string() << '\n';
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

    return true;
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

    bool hasFailure = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::filesystem::path samplePath = argv[i];
        try
        {
            const MusicTag tag = TagReader::Read(samplePath, coverExportDir);
            PrintTagSummary(samplePath, tag);
            if (!RunCoverCacheSmoke(samplePath, coverExportDir, tag))
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
