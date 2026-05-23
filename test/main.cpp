#include "TagReader.hpp"

#include <iostream>
#include <filesystem>
#include <string>

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

    return 0;
}
