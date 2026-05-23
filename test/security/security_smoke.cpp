#include "TagReader.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

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
} // namespace

int main(int argc, char **argv)
{
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
