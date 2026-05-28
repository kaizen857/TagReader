#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct AVFormatContext;

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);
    static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);

private:
    enum class DetectedContainer
    {
        Unknown,
        Mp3,
        Flac,
        OggVorbis,
        Mp4,
    };

    struct ReadContext
    {
        struct FormatContextDeleter
        {
            void operator()(AVFormatContext *context) const noexcept;
        };

        std::filesystem::path filePath;
        std::filesystem::path coverExportDir;
        std::ifstream input;
        std::uintmax_t fileSize{};
        std::filesystem::file_time_type lastModified{};
        std::unique_ptr<AVFormatContext, FormatContextDeleter> formatContext;
        int audioStreamIndex{-1};
        DetectedContainer detectedContainer{DetectedContainer::Unknown};
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

    struct Id3TagView
    {
        uint8_t versionMajor{};
        uint8_t flags{};
        bool tagUnsync{};
        std::size_t cursor{};
        std::size_t limit{};
        std::vector<uint8_t> bytes;
    };

private:
    static void ValidatePath(const std::filesystem::path &filePath);
    static void ValidateCoverExportDir(const std::filesystem::path &coverExportDir);
    static ReadContext OpenContext(const std::filesystem::path &filePath);
    static void DetectStream(ReadContext &context);
    static RawMediaInfo ReadMediaInfo(const ReadContext &context);
    static RawMetadata ReadMetadata(ReadContext &context);
    static RawLyrics ReadLyrics(ReadContext &context);
    static DecodedField NormalizeText(std::string_view value);
    static DetectedContainer DetectContainer(ReadContext &context);
    static std::string NormalizeContainerFormatName(const ReadContext &context);
    static std::string DetectTextEncoding(std::string_view raw);
    static DecodedField DecodeTextToUtf8(std::string_view raw, std::string_view encoding);
    static DecodedField DecodeRawText(std::string_view raw);
    static void NormalizeMetadata(RawMetadata &metadata);
    static void NormalizeLyrics(RawLyrics &lyrics);
    static MusicTag BuildMusicTag(const ReadContext &context, const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics);

    static void ReadID3v1Metadata(ReadContext &context, RawMetadata &metadata);
    static void ReadID3v2Metadata(ReadContext &context, RawMetadata &metadata);
    static bool ReadId3TagBytes(ReadContext &context, Id3TagView &tagView);
    static void ReadID3v22Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, std::size_t cursor);
    static void ReadID3v23Or24Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit);
    static void ReadID3v22Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v22PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v2Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v2PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize);
    static void ReadID3v2ApicPayload(ReadContext &context, RawMetadata &metadata, std::string_view mimeType, uint8_t pictureType, const uint8_t *imageData, std::size_t imageSize);
    static void ReadVorbisCommentMetadata(ReadContext &context, RawMetadata &metadata);
    static void ReadVorbisCommentEntry(RawMetadata &metadata, std::string_view entry);
    static void ReadOggVorbisComments(ReadContext &context, RawMetadata &metadata);
    static bool ReadOggVorbisCommentEntries(ReadContext &context, const std::function<void(std::string_view)> &handler);
    static void ReadFlacMetadataBlocks(ReadContext &context, RawMetadata &metadata);
    static void ReadFlacPictureEntry(ReadContext &context, RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize);
    static void ReadMP4Metadata(ReadContext &context, RawMetadata &metadata);
    static void ReadMP4AtomTree(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t limit);
    static void ReadMP4ItemAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms);
    static DecodedField DecodeMp4TextData(std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize);
    static std::optional<std::uint16_t> ParseMp4TrackDiskNumber(const uint8_t *payload, std::size_t payloadSize);
    static void ReadMP4DataAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize);

    static void ReadID3Lyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadVorbisLyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadVorbisLyricsEntry(RawLyrics &lyrics, std::string_view key, std::string_view value);
    static void ReadLyricsFromPlainText(RawLyrics &lyrics, std::string_view text);
    static void ReadMP4Lyrics(ReadContext &context, RawLyrics &lyrics);
    static void ReadMP4LyricsAtomTree(ReadContext &context, RawLyrics &lyrics, std::uintmax_t offset, std::uintmax_t limit);
    static void ReadMP4LyricsItem(ReadContext &context, RawLyrics &lyrics, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms);
    static void ReadMP4FreeformLyricsItem(ReadContext &context, RawLyrics &lyrics, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms);
    static void AppendPlainLyrics(RawLyrics &lyrics, std::string text);
    static void AppendTimedLyrics(RawLyrics &lyrics, std::chrono::microseconds timestamp, std::string text);
    static bool ParseLrcTimestamp(std::string_view token, std::chrono::microseconds &timestamp);
    static void ReadID3v22LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, std::size_t cursor);
    static void ReadID3v23Or24LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit);
};

#endif
