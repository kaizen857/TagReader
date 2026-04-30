#include "TagReader.hpp"

#include <iostream>
#include <filesystem>
#include <string>

namespace
{
void PrintTag(const MusicTag &tag)
{
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
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <audio-file-path>\n";
        return 1;
    }

    try
    {
        const MusicTag tag = TagReader::Read(std::filesystem::path(argv[1]));
        PrintTag(tag);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "TagReader error: " << ex.what() << '\n';
        return 2;
    }

    return 0;
}
