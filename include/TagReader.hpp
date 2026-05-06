#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct AVFormatContext;

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);

private:
    struct ReadContext
    {
        struct FormatContextDeleter
        {
            void operator()(AVFormatContext *context) const noexcept;
        };

        std::filesystem::path filePath;
        std::ifstream input;
        std::uintmax_t fileSize{};
        std::filesystem::file_time_type lastModified{};
        std::unique_ptr<AVFormatContext, FormatContextDeleter> formatContext;
        int audioStreamIndex{-1};
        std::string containerName;
        std::string containerLongName;
        std::vector<std::string> metadataSourcePriority;
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
    static void DetectStream(ReadContext &context);
    static RawMediaInfo ReadMediaInfo(const ReadContext &context);
    static RawMetadata ReadMetadata(ReadContext &context);
    static RawLyrics ReadLyrics(ReadContext &context);
    static DecodedField NormalizeText(std::string_view value);
    static void NormalizeMetadata(RawMetadata &metadata);
    static void NormalizeLyrics(RawLyrics &lyrics);
    static MusicTag BuildMusicTag(const ReadContext &context, const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics);

    static void ReadID3v1Metadata(ReadContext &context, RawMetadata &metadata);
    static void ReadID3v2Metadata(ReadContext &context, RawMetadata &metadata);
    static void ReadID3v2Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v2PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v2ApicPayload(ReadContext &context, RawMetadata &metadata, std::string_view mimeType, const uint8_t *imageData, std::size_t imageSize);
    static void ReadVorbisCommentMetadata(ReadContext &context, RawMetadata &metadata);
    static void ReadVorbisCommentBlock(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t size);
    static void ReadVorbisCommentEntry(RawMetadata &metadata, std::string_view entry);
    static void ReadOggVorbisComments(ReadContext &context, RawMetadata &metadata);
    static void ReadFlacPictureBlock(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t size);
    static void ReadFlacPictureEntry(ReadContext &context, RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize);
    static void ReadMP4Metadata(ReadContext &context, RawMetadata &metadata);
    static void ReadMP4AtomTree(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t limit, std::uint32_t depth = 0);
    static void ReadMP4ItemAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit);
    static void ReadMP4DataAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize);
    static void ExtractCoverToTempFile(ReadContext &context, RawMetadata &metadata);

    static void ReadID3Lyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadVorbisLyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadVorbisLyricsEntry(RawLyrics &lyrics, std::string_view key, std::string_view value);
    static void ReadLyricsFromPlainText(RawLyrics &lyrics, std::string_view text);
    static void ReadMP4Lyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadMP4LyricsItem(ReadContext &context, RawLyrics &lyrics, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit);
    static void AppendPlainLyrics(RawLyrics &lyrics, std::string text);
    static void AppendTimedLyrics(RawLyrics &lyrics, std::chrono::microseconds timestamp, std::string text);
    static bool ParseLrcTimestamp(std::string_view token, std::chrono::microseconds &timestamp);
};

#endif
