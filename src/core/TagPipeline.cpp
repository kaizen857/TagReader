#include "core/TagPipeline.hpp"

#include "formats/flac/FlacParser.hpp"
#include "formats/id3/Id3Parser.hpp"
#include "formats/mp4/Mp4Parser.hpp"
#include "formats/ogg-vorbis/OggVorbisParser.hpp"
#include "media/ContainerDetector.hpp"
#include "media/FfmpegSession.hpp"
#include "media/MediaInfoReader.hpp"
#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

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
    if (coverExportDir.empty())
    {
        return;
    }

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
}

RawMetadata ReadMetadata(ReadContext &context, TagFormat tagFormat)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }

    RawMetadata metadata{};
    auto ignoreMalformedMetadata = [](auto &&readMetadata)
    {
        try
        {
            readMetadata();
        }
        catch (const std::filesystem::filesystem_error &)
        {
        }
        catch (const std::runtime_error &ex)
        {
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
    case TagFormat::Id3v2:
        // ID3v2 is authoritative, but ID3v1 may still fill fields absent from the leading tag.
        ignoreMalformedMetadata([&]()
                                { tagreader_id3::ReadID3v2Metadata(context, metadata); });
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
        case TagFormat::Unknown:
            break;
        }

        NormalizeLyrics(lyrics);
    }
    catch (const std::filesystem::filesystem_error &)
    {
        lyrics = {};
    }
    catch (const std::runtime_error &)
    {
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
    context.coverExportDir = coverExportDir;
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
