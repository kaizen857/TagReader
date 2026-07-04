#ifndef TAGREADER_CORE_RAWTAGDATA_HPP
#define TAGREADER_CORE_RAWTAGDATA_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace tagreader_core
{
struct RawMediaInfo
{
    int64_t duration{};
    int64_t offset{};
    uint32_t sampleRate{};
    uint32_t bitDepth{};
    uint32_t bitRate{};
    uint8_t channels{};
    std::string format;
};

struct RawMetadata
{
    std::string title;
    std::string genre;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string composer;
    std::string comment;
    uint16_t year{};
    uint16_t trackNumber{};
    uint16_t discNumber{};
    uint32_t playCount{};
    uint8_t rating{};
    std::filesystem::path coverPath;
    std::filesystem::path thumbnailPath;
};

struct RawLyrics
{
    std::vector<std::pair<std::chrono::microseconds, std::string>> timedLines;
    std::string text;
};

struct DecodedField
{
    std::string value;
    std::string encoding;
    bool success{};
};

struct Id3TagView
{
    uint8_t versionMajor{};
    uint8_t flags{};
    bool tagUnsync{};
    std::size_t cursor{};
    std::size_t limit{};
    std::vector<uint8_t> bytes;
};
}

#endif
