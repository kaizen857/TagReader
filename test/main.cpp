#include "TagReader.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/log.h>
#ifdef __cplusplus
}
#endif

#include <iostream>
#include <filesystem>
#include <string>

#ifdef TAGREADER_ENABLE_PROFILING
#include <tracy/tracy/Tracy.hpp>
#include <thread>
#include <chrono>
#endif

namespace
{
void PrintTag(const MusicTag &tag)
{
    // 逐字段打印，便于人工确认解析结果和默认值是否符合预期。
    std::cout << "filePath: " << tag.filePath().string() << '\n';
    std::cout << "title: " << tag.title() << '\n';
    std::cout << "artist: " << tag.artist() << '\n';
    std::cout << "album: " << tag.album() << '\n';
    std::cout << "albumArtist: " << tag.albumArtist() << '\n';
    std::cout << "composer: " << tag.composer() << '\n';
    std::cout << "genre: " << tag.genre() << '\n';
    std::cout << "format: " << tag.format() << '\n';
    std::cout << "year: " << tag.year() << '\n';
    std::cout << "trackNumber: " << tag.trackNumber() << '\n';
    std::cout << "discNumber: " << tag.discNumber() << '\n';
    std::cout << "duration(us): " << tag.duration() << '\n';
    std::cout << "offset(us): " << tag.offset() << '\n';
    std::cout << "sampleRate: " << tag.sampleRate() << '\n';
    std::cout << "bitDepth: " << tag.bitDepth() << '\n';
    std::cout << "bitRate: " << tag.bitRate() << '\n';
    std::cout << "channels: " << static_cast<unsigned>(tag.channels()) << '\n';
    std::cout << "playCount: " << tag.playCount() << '\n';
    std::cout << "rating: " << static_cast<unsigned>(tag.rating()) << '\n';
    std::cout << "coverPath: " << tag.coverPath().string() << '\n';
    std::cout << "lyricsCount: " << tag.lyrics().size() << '\n';
    std::cout << "lyrics:\n";
    for (const auto &lyric : tag.lyrics().lyrics())
    {
        std::cout << "[" << lyric.timestamp() << "]:" << lyric.text() << '\n';
    }
}
} // namespace

int main(int argc, char *argv[])
{
    av_log_set_level(AV_LOG_QUIET);

#ifdef TAGREADER_ENABLE_PROFILING
    std::cout << "Tracy profiling enabled. Waiting for profiler connection...\n";
    std::cout << "Please connect Tracy Profiler within 10 seconds.\n";
    
    // 等待 Tracy Profiler 连接（最多 10 秒）
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(10);
    
    while (!tracy::ProfilerAvailable())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= timeout)
        {
            std::cout << "Tracy Profiler not connected after 10 seconds. Proceeding anyway.\n";
            break;
        }
    }
    
    if (tracy::ProfilerAvailable())
    {
        std::cout << "Tracy Profiler connected! Starting measurement...\n";
    }
#endif

    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <audio-file-path> [cover-export-dir]\n";
        return 1;
    }

    try
    {
        const MusicTag tag = argc >= 3
                                 ? TagReader::Read(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]))
                                 : TagReader::Read(std::filesystem::path(argv[1]));
        PrintTag(tag);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "TagReader error: " << ex.what() << '\n';
        return 2;
    }

#ifdef TAGREADER_ENABLE_PROFILING
    std::cout << "\nProfiling complete. Waiting for Tracy to receive data...\n";
    // 给 Tracy 时间传输数据
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif

    return 0;
}
