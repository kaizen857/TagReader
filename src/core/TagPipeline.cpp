#include "core/TagPipeline.hpp"

#include "formats/flac/FlacParser.hpp"
#include "formats/id3/Id3Parser.hpp"
#include "formats/mp4/Mp4Parser.hpp"
#include "formats/ogg-vorbis/OggVorbisParser.hpp"
#include "formats/ape/ApeParser.hpp"
#include "media/ContainerDetector.hpp"
#include "media/FfmpegSession.hpp"
#include "media/MediaInfoReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>

namespace tagreader_core
{
namespace
{
using tagreader_text::NormalizeLyrics;
using tagreader_text::NormalizeMetadata;
using tagreader_text::TrimText;

bool IsCoverExportOrCacheError(std::string_view message)
{
    return message.find("cover export") != std::string_view::npos || message.find("cover cache") != std::string_view::npos;
}

std::filesystem::path DefaultCoverExportDir()
{
    std::error_code ec;
    std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        throw std::runtime_error("failed to query default cover export directory: " + ec.message());
    }
    return tempRoot / "tagreader-covers";
}

std::filesystem::path MakeCoverExportProbePath(const std::filesystem::path &coverExportDir)
{
    static std::atomic_uint64_t probeCounter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto seq = probeCounter.fetch_add(1, std::memory_order_relaxed);
    return coverExportDir / (".tagreader-cover-export-probe." + std::to_string(now) + "." + std::to_string(seq));
}

class StreamStateGuard
{
public:
    explicit StreamStateGuard(std::ifstream &stream) noexcept : stream_(stream) {}
    ~StreamStateGuard() { stream_.clear(); }
    StreamStateGuard(const StreamStateGuard &) = delete;
    StreamStateGuard &operator=(const StreamStateGuard &) = delete;
private:
    std::ifstream &stream_;
};

void ValidatePath(const std::filesystem::path &filePath)
{
    if (filePath.empty())
    {
        throw std::invalid_argument("file path is empty");
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query file existence: " + ec.message());
    }
    if (!exists)
    {
        throw std::runtime_error("file does not exist: " + filePath.string());
    }

    const bool regularFile = std::filesystem::is_regular_file(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query file type: " + ec.message());
    }
    if (!regularFile)
    {
        throw std::runtime_error("path is not a regular file: " + filePath.string());
    }
}

void ValidateCoverExportDir(const std::filesystem::path &coverExportDir)
{
    std::error_code ec;
    std::filesystem::create_directories(coverExportDir, ec);
    if (ec)
    {
        throw std::runtime_error("failed to create cover export directory: " + ec.message());
    }

    const bool exists = std::filesystem::exists(coverExportDir, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query cover export directory: " + ec.message());
    }
    if (!exists)
    {
        throw std::runtime_error("cover export directory does not exist: " + coverExportDir.string());
    }

    const bool directory = std::filesystem::is_directory(coverExportDir, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query cover export directory type: " + ec.message());
    }
    if (!directory)
    {
        throw std::runtime_error("cover export path is not a directory: " + coverExportDir.string());
    }

    const std::filesystem::path probePath = MakeCoverExportProbePath(coverExportDir);
    auto removeProbe = [&probePath]() noexcept
    {
        std::error_code removeEc;
        std::filesystem::remove(probePath, removeEc);
    };

    removeProbe();
    {
        std::ofstream output(probePath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            removeProbe();
            throw std::runtime_error("cover export directory is not writable: " + coverExportDir.string());
        }
        output << "tagreader-cover-export-probe\n";
        if (!output.good())
        {
            removeProbe();
            throw std::runtime_error("cover export probe write failed: " + coverExportDir.string());
        }
    }

    {
        std::ifstream input(probePath, std::ios::binary);
        std::string probe;
        if (!input || !std::getline(input, probe) || probe != "tagreader-cover-export-probe")
        {
            removeProbe();
            throw std::runtime_error("cover export probe read failed: " + coverExportDir.string());
        }
    }

    std::filesystem::remove(probePath, ec);
    if (ec)
    {
        removeProbe();
        throw std::runtime_error("cover export probe delete failed: " + coverExportDir.string() + ": " + ec.message());
    }
}

RawMetadata ReadMetadata(ReadContext &context, TagFormat tagFormat)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }

    RawMetadata metadata{};
    context.input.clear();
    auto ignoreMalformedMetadata = [&context](auto &&readMetadata)
    {
        try
        {
            StreamStateGuard guard(context.input);
            readMetadata();
        }
        catch (const std::filesystem::filesystem_error &ex)
        {
            if (context.diagnostics != nullptr)
            {
                *context.diagnostics << "parser metadata error: " << ex.what() << '\n';
            }
        }
        catch (const std::runtime_error &ex)
        {
            if (context.diagnostics != nullptr)
            {
                *context.diagnostics << "parser metadata error: " << ex.what() << '\n';
            }
            if (IsCoverExportOrCacheError(ex.what()))
            {
                throw;
            }
        }
    };

    switch (tagFormat)
    {
    case TagFormat::Mp4:
        ignoreMalformedMetadata([&]()
                                { tagreader_mp4::ReadMp4Metadata(context, metadata); });
        break;
    case TagFormat::Flac:
    case TagFormat::VorbisComment:
        ignoreMalformedMetadata([&]()
                                { tagreader_flac::ReadFlacMetadata(context, metadata); });
        break;
    case TagFormat::OggVorbis:
        ignoreMalformedMetadata([&]()
                                { tagreader_ogg_vorbis::ReadOggVorbisMetadata(context, metadata); });
        break;
    case TagFormat::Ape:
        ignoreMalformedMetadata([&]()
                                { tagreader_ape::ReadApeMetadata(context, metadata); });
        context.input.clear();
        // MP3+APE: try ID3v2 then ID3v1 for fields APE did not provide.
        {
            std::string containerLower = context.containerName;
            std::transform(containerLower.begin(), containerLower.end(), containerLower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (containerLower.find("mp3") != std::string::npos ||
                containerLower.find("mpeg") != std::string::npos)
            {
                ignoreMalformedMetadata([&]()
                                        { tagreader_id3::ReadID3v2Metadata(context, metadata); });
                context.input.clear();
                ignoreMalformedMetadata([&]()
                                        { tagreader_id3::ReadID3v1Metadata(context, metadata); });
            }
        }
        break;
    case TagFormat::Id3v2:
        // ID3v2 is authoritative, but ID3v1 may still fill fields absent from the leading tag.
        ignoreMalformedMetadata([&]()
                                { tagreader_id3::ReadID3v2Metadata(context, metadata); });
        context.input.clear();
        ignoreMalformedMetadata([&]()
                                { tagreader_id3::ReadID3v1Metadata(context, metadata); });
        break;
    case TagFormat::Id3v1:
    case TagFormat::Unknown:
        ignoreMalformedMetadata([&]()
                                { tagreader_id3::ReadID3v1Metadata(context, metadata); });
        break;
    }

    // 评分和播放次数保持固定值，不参与元数据读取。
    metadata.playCount = 0;
    metadata.rating = 0;

    NormalizeMetadata(metadata);

    return metadata;
}

RawLyrics ReadLyrics(ReadContext &context, TagFormat tagFormat)
{
    RawLyrics lyrics{};
    if (!context.input.is_open())
    {
        return lyrics;
    }

    context.input.clear();

    try
    {
        switch (tagFormat)
        {
        case TagFormat::Id3v1:
        case TagFormat::Id3v2:
            tagreader_id3::ReadID3Lyrics(context, lyrics);
            break;
        case TagFormat::Flac:
        case TagFormat::VorbisComment:
            tagreader_flac::ReadFlacLyrics(context, lyrics);
            break;
        case TagFormat::OggVorbis:
            tagreader_ogg_vorbis::ReadOggVorbisLyrics(context, lyrics);
            break;
        case TagFormat::Mp4:
            tagreader_mp4::ReadMp4Lyrics(context, lyrics);
            break;
        case TagFormat::Ape:
            tagreader_ape::ReadApeLyrics(context, lyrics);
            break;
        case TagFormat::Unknown:
            break;
        }

        NormalizeLyrics(lyrics);
    }
    catch (const std::filesystem::filesystem_error &ex)
    {
        if (context.diagnostics != nullptr)
        {
            *context.diagnostics << "parser lyrics error: " << ex.what() << '\n';
        }
        lyrics = {};
    }
    catch (const std::runtime_error &ex)
    {
        if (context.diagnostics != nullptr)
        {
            *context.diagnostics << "parser lyrics error: " << ex.what() << '\n';
        }
        lyrics = {};
    }

    return lyrics;
}

MusicTag BuildMusicTag(const ReadContext &context, const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics)
{
    MusicTag tag{};

    tag.setTitle(metadata.title);
    tag.setGenre(metadata.genre);
    tag.setArtist(metadata.artist);
    tag.setAlbum(metadata.album);
    tag.setAlbumArtist(metadata.albumArtist);
    tag.setComposer(metadata.composer);
    tag.setYear(metadata.year);
    tag.setTrackNumber(metadata.trackNumber);
    tag.setDiscNumber(metadata.discNumber);

    Lyrics outLyrics{};
    if (!lyrics.timedLines.empty())
    {
        for (const auto &line : lyrics.timedLines)
        {
            outLyrics.addLyric(Lyric(line.first, line.second));
        }
    }
    else if (!lyrics.text.empty())
    {
        std::size_t start = 0;
        while (start <= lyrics.text.size())
        {
            const std::size_t end = lyrics.text.find('\n', start);
            const std::string_view line = end == std::string::npos ? std::string_view(lyrics.text).substr(start) : std::string_view(lyrics.text).substr(start, end - start);
            const std::string trimmed = TrimText(std::string(line));
            if (!trimmed.empty())
            {
                outLyrics.addLyric(Lyric(std::chrono::microseconds{0}, trimmed));
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }
    tag.setLyrics(std::move(outLyrics));

    tag.setFilePath(context.filePath);
    tag.setCoverPath(metadata.coverPath);
    tag.setDuration(mediaInfo.duration);
    tag.setOffset(mediaInfo.offset);
    tag.setLastModified(context.lastModified);
    tag.setSampleRate(mediaInfo.sampleRate);
    tag.setBitDepth(mediaInfo.bitDepth);
    tag.setBitRate(mediaInfo.bitRate);
    tag.setChannels(mediaInfo.channels);
    tag.setFormat(mediaInfo.format);
    tag.setPlayCount(metadata.playCount);
    tag.setRating(metadata.rating);

    return tag;
}
}

MusicTag ReadTag(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    tagreader_media::RegisterAllFormatsIfNeeded();

    ValidatePath(filePath);

    ReadContext context = tagreader_media::OpenContext(filePath);
    context.coverExportDir = coverExportDir.empty() ? DefaultCoverExportDir() : coverExportDir;
    ValidateCoverExportDir(context.coverExportDir);
    tagreader_media::DetectStream(context);

    const TagFormat tagFormat = tagreader_media::DetectTagFormat(context);
    // Use raw-byte tag detection for parser dispatch and user-facing format normalization.
    context.detectedContainer = tagreader_media::ContainerFromTagFormat(tagFormat);

    const RawMediaInfo mediaInfo = tagreader_media::ReadMediaInfo(context);
    const RawMetadata metadata = ReadMetadata(context, tagFormat);
    const RawLyrics lyrics = ReadLyrics(context, tagFormat);

    return BuildMusicTag(context, mediaInfo, metadata, lyrics);
}
}
