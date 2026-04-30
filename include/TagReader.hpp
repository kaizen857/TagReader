#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);

private:
    struct ReadContext
    {
        std::filesystem::path filePath;
    };

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
        uint16_t year{};
        uint16_t trackNumber{};
        uint16_t discNumber{};
        uint32_t playCount{};
        uint8_t rating{};
        std::filesystem::path coverPath;
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

private:
    static void ValidatePath(const std::filesystem::path &filePath);
    static ReadContext OpenContext(const std::filesystem::path &filePath);
    static void DetectStream(const ReadContext &context);
    static RawMediaInfo ReadMediaInfo(const ReadContext &context);
    static RawMetadata ReadMetadata(const ReadContext &context);
    static RawLyrics ReadLyrics(const ReadContext &context);
    static DecodedField NormalizeText(std::string_view value);
    static MusicTag BuildMusicTag(const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics);
};

#endif
