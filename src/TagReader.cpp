#include "TagReader.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <stdexcept>

void TagReader::ReadContext::FormatContextDeleter::operator()(AVFormatContext *context) const noexcept
{
    if (context != nullptr)
    {
        avformat_close_input(&context);
    }
}

namespace
{
[[noreturn]] void NotImplemented(const char *name)
{
    throw std::logic_error(std::string(name) + " is not implemented yet");
}

std::string MakeFFmpegError(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string TrimText(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n\0");
    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n\0");
    return value.substr(first, last - first + 1);
}

std::string MakeTempCoverPath()
{
    static std::atomic_uint64_t counter{0};

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        throw std::runtime_error("failed to locate temp directory: " + ec.message());
    }

    const auto now = std::filesystem::file_time_type::clock::now().time_since_epoch().count();
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);

    return (tempDir / ("tagreader_cover_" + std::to_string(now) + "_" + std::to_string(seq) + ".jpg")).string();
}

void WriteBinaryFile(const std::filesystem::path &path, const uint8_t *data, std::size_t size)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        throw std::runtime_error("failed to create cover file: " + path.string());
    }

    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!out)
    {
        throw std::runtime_error("failed to write cover file: " + path.string());
    }
}
} // namespace

MusicTag TagReader::Read(const std::filesystem::path &filePath)
{
    // FFmpeg 的全局日志保持静默，避免测试程序污染终端输出。
    av_log_set_level(AV_LOG_QUIET);
#if LIBAVFORMAT_VERSION_MAJOR < 59
    av_register_all();
#endif

    ValidatePath(filePath);

    ReadContext context = OpenContext(filePath);
    DetectStream(context);

    const RawMediaInfo mediaInfo = ReadMediaInfo(context);
    const RawMetadata metadata = ReadMetadata(context);
    const RawLyrics lyrics = ReadLyrics(context);

    return BuildMusicTag(mediaInfo, metadata, lyrics);
}

void TagReader::ValidatePath(const std::filesystem::path &filePath)
{
    // 先做最早期的输入过滤，避免把明显无效的路径带进后续解封装流程。
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

    const auto status = std::filesystem::status(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query file status: " + ec.message());
    }

    const auto perms = status.permissions();
    constexpr auto readMask = std::filesystem::perms::owner_read |
                               std::filesystem::perms::group_read |
                               std::filesystem::perms::others_read;
    if ((perms & readMask) == std::filesystem::perms::none)
    {
        throw std::runtime_error("file is not readable: " + filePath.string());
    }
}

TagReader::ReadContext TagReader::OpenContext(const std::filesystem::path &filePath)
{
    ReadContext context{};
    context.filePath = filePath;

    // 这里先记录文件状态，后续元数据提取需要回读原始文件尾部。
    std::error_code ec;
    context.fileSize = std::filesystem::file_size(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query file size: " + ec.message());
    }

    context.lastModified = std::filesystem::last_write_time(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query file modification time: " + ec.message());
    }

    context.input.open(filePath, std::ios::binary);
    if (!context.input.is_open())
    {
        throw std::runtime_error("failed to open file input stream");
    }

    // 交给 FFmpeg 完成实际 probe 和流信息探测。
    AVFormatContext *formatContext = nullptr;
    const int openResult = avformat_open_input(&formatContext, filePath.string().c_str(), nullptr, nullptr);
    if (openResult < 0)
    {
        throw std::runtime_error("avformat_open_input failed: " + MakeFFmpegError(openResult));
    }

    const int infoResult = avformat_find_stream_info(formatContext, nullptr);
    if (infoResult < 0)
    {
        avformat_close_input(&formatContext);
        throw std::runtime_error("avformat_find_stream_info failed: " + MakeFFmpegError(infoResult));
    }

    context.formatContext.reset(formatContext);

    return context;
}

void TagReader::DetectStream(ReadContext &context)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }

    // 这一步只做容器识别和主音频流定位，不读取任何标签内容。
    const AVFormatContext *formatContext = context.formatContext.get();
    context.audioStreamIndex = -1;
    context.containerName.clear();
    context.containerLongName.clear();
    context.metadataSourcePriority.clear();

    if (formatContext->iformat != nullptr)
    {
        if (formatContext->iformat->name != nullptr)
        {
            context.containerName = formatContext->iformat->name;
        }
        if (formatContext->iformat->long_name != nullptr)
        {
            context.containerLongName = formatContext->iformat->long_name;
        }
    }

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i)
    {
        const AVStream *stream = formatContext->streams[i];
        if (stream == nullptr || stream->codecpar == nullptr)
        {
            continue;
        }

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            context.audioStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (context.audioStreamIndex < 0)
    {
        throw std::runtime_error("no audio stream found in input file");
    }

    if (!context.containerName.empty())
    {
        context.metadataSourcePriority.push_back(context.containerName);
    }
    if (!context.containerLongName.empty() && context.containerLongName != context.containerName)
    {
        context.metadataSourcePriority.push_back(context.containerLongName);
    }

    const AVStream *audioStream = formatContext->streams[context.audioStreamIndex];
    if (audioStream == nullptr || audioStream->codecpar == nullptr)
    {
        throw std::runtime_error("audio stream information is incomplete");
    }
}

namespace
{
std::string GetDictionaryValue(const AVDictionary *dict, const char *key)
{
    const AVDictionaryEntry *entry = av_dict_get(dict, key, nullptr, 0);
    if (entry == nullptr || entry->value == nullptr)
    {
        return {};
    }
    return TrimText(entry->value);
}

uint16_t ParseUInt16(const std::string &value)
{
    if (value.empty())
    {
        return 0;
    }

    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed == 0)
        {
            return 0;
        }
        return static_cast<uint16_t>(parsed);
    }
    catch (...) {
        return 0;
    }
}

std::pair<uint16_t, uint16_t> ParseSlashNumber(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos)
    {
        return {ParseUInt16(value), 0};
    }
    return {ParseUInt16(value.substr(0, slash)), ParseUInt16(value.substr(slash + 1))};
}
} // namespace

TagReader::RawMediaInfo TagReader::ReadMediaInfo(const ReadContext &context)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }
    if (context.audioStreamIndex < 0)
    {
        throw std::runtime_error("audio stream index is not initialized");
    }

    const AVFormatContext *formatContext = context.formatContext.get();
    const AVStream *audioStream = formatContext->streams[context.audioStreamIndex];
    if (audioStream == nullptr || audioStream->codecpar == nullptr)
    {
        throw std::runtime_error("audio stream information is incomplete");
    }

    RawMediaInfo mediaInfo{};

    // 时长和偏移优先使用容器级信息，不足时回退到音频流级信息。
    if (formatContext->duration != AV_NOPTS_VALUE)
    {
        mediaInfo.duration = av_rescale_q(formatContext->duration, AV_TIME_BASE_Q, AVRational{1, 1000000});
    }
    else if (audioStream->duration != AV_NOPTS_VALUE)
    {
        mediaInfo.duration = av_rescale_q(audioStream->duration, audioStream->time_base, AVRational{1, 1000000});
    }

    if (formatContext->start_time != AV_NOPTS_VALUE)
    {
        mediaInfo.offset = av_rescale_q(formatContext->start_time, AV_TIME_BASE_Q, AVRational{1, 1000000});
    }
    else if (audioStream->start_time != AV_NOPTS_VALUE)
    {
        mediaInfo.offset = av_rescale_q(audioStream->start_time, audioStream->time_base, AVRational{1, 1000000});
    }

    mediaInfo.sampleRate = audioStream->codecpar->sample_rate > 0
                               ? static_cast<uint32_t>(audioStream->codecpar->sample_rate)
                               : 0;

    // 比特率优先取音频流自身值，缺失时退回容器级比特率。
    mediaInfo.bitRate = audioStream->codecpar->bit_rate > 0
                            ? static_cast<uint32_t>(audioStream->codecpar->bit_rate)
                            : (formatContext->bit_rate > 0 ? static_cast<uint32_t>(formatContext->bit_rate) : 0);

#if LIBAVCODEC_VERSION_MAJOR >= 59
    mediaInfo.channels = audioStream->codecpar->ch_layout.nb_channels > 0
                             ? static_cast<uint8_t>(audioStream->codecpar->ch_layout.nb_channels)
                             : 0;
#else
    mediaInfo.channels = audioStream->codecpar->channels > 0
                             ? static_cast<uint8_t>(audioStream->codecpar->channels)
                             : 0;
#endif

    mediaInfo.bitDepth = audioStream->codecpar->bits_per_coded_sample > 0
                             ? static_cast<uint32_t>(audioStream->codecpar->bits_per_coded_sample)
                             : 0;

    // 格式名优先使用前面探测阶段保存的容器短名。
    if (!context.containerName.empty())
    {
        mediaInfo.format = context.containerName;
    }
    else if (formatContext->iformat != nullptr && formatContext->iformat->name != nullptr)
    {
        mediaInfo.format = formatContext->iformat->name;
    }

    return mediaInfo;
}

TagReader::RawMetadata TagReader::ReadMetadata(ReadContext &context)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }

    RawMetadata metadata{};
    const std::string container = ToLower(context.containerName);

    // 这里只做容器分发，不承载具体字段解析逻辑。
    if (container.find("mp4") != std::string::npos || container.find("mov") != std::string::npos || container.find("m4") != std::string::npos)
    {
        ReadMP4Metadata(context, metadata);
    }
    else if (container.find("ogg") != std::string::npos || container.find("flac") != std::string::npos || container.find("vorbis") != std::string::npos)
    {
        ReadVorbisCommentMetadata(context, metadata);
    }
    else if (container.find("mp3") != std::string::npos || container.find("mpeg") != std::string::npos)
    {
        ReadID3v2Metadata(context, metadata);
        ReadID3v1Metadata(context, metadata);
    }
    else
    {
        // 未知容器先尝试最常见的尾部标签，失败则保持空值。
        ReadID3v1Metadata(context, metadata);
    }

    // 评分和播放次数保持固定值，不参与元数据读取。
    metadata.playCount = 0;
    metadata.rating = 0;

    ExtractCoverToTempFile(context, metadata);

    return metadata;
}

void TagReader::ReadID3v1Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open() || context.fileSize < 128)
    {
        return;
    }

    // ID3v1 只在文件尾部固定 128 字节内读取，适合做轻量补充。
    std::array<char, 128> buffer{};
    context.input.clear();
    context.input.seekg(-128, std::ios::end);
    if (!context.input)
    {
        return;
    }

    context.input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (context.input.gcount() != static_cast<std::streamsize>(buffer.size()))
    {
        return;
    }

    if (std::string_view(buffer.data(), 3) != "TAG")
    {
        return;
    }

    auto readField = [&](std::size_t offset, std::size_t size) {
        return TrimText(std::string(buffer.data() + offset, size));
    };

    if (metadata.title.empty())
    {
        metadata.title = readField(3, 30);
    }
    if (metadata.artist.empty())
    {
        metadata.artist = readField(33, 30);
    }
    if (metadata.album.empty())
    {
        metadata.album = readField(63, 30);
    }
    if (metadata.genre.empty())
    {
        metadata.genre = readField(127, 1);
    }

    const std::string comment = readField(97, 30);
    if (metadata.trackNumber == 0 && buffer[125] == '\0')
    {
        metadata.trackNumber = static_cast<uint16_t>(static_cast<unsigned char>(buffer[126]));
    }
    if (metadata.composer.empty())
    {
        metadata.composer = comment;
    }
}

void TagReader::ReadID3v2Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    // 这里先保留为格式入口，后续按帧头/帧遍历继续拆分。
    (void)context;
    (void)metadata;
}

void TagReader::ReadVorbisCommentMetadata(ReadContext &context, RawMetadata &metadata)
{
    if (context.formatContext == nullptr)
    {
        return;
    }

    // Vorbis Comment 需要直接从文件容器内容读取，避免依赖 FFmpeg 通用 metadata 表。
    (void)context;
    (void)metadata;
}

void TagReader::ReadMP4Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (context.formatContext == nullptr)
    {
        return;
    }

    // MP4 / M4A 的 atom/box 解析后续会继续拆分，这里先保留明确的格式入口。
    (void)context;
    (void)metadata;
}

void TagReader::ExtractCoverToTempFile(ReadContext &context, RawMetadata &metadata)
{
    if (context.formatContext == nullptr)
    {
        return;
    }

    const AVFormatContext *formatContext = context.formatContext.get();
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i)
    {
        const AVStream *stream = formatContext->streams[i];
        if (stream == nullptr || stream->codecpar == nullptr)
        {
            continue;
        }

        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
        {
            continue;
        }

        const AVPacket &packet = stream->attached_pic;
        if (packet.data == nullptr || packet.size <= 0)
        {
            continue;
        }

        // 封面先落盘到临时目录，再把路径写回元数据结构。
        const std::string tempPath = MakeTempCoverPath();
        WriteBinaryFile(tempPath, packet.data, static_cast<std::size_t>(packet.size));
        metadata.coverPath = tempPath;
        return;
    }
}

TagReader::RawLyrics TagReader::ReadLyrics(const ReadContext &context)
{
    (void)context;
    NotImplemented("TagReader::ReadLyrics");
    return {};
}

TagReader::DecodedField TagReader::NormalizeText(std::string_view value)
{
    (void)value;
    NotImplemented("TagReader::NormalizeText");
    return {};
}

MusicTag TagReader::BuildMusicTag(const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics)
{
    (void)mediaInfo;
    (void)metadata;
    (void)lyrics;
    NotImplemented("TagReader::BuildMusicTag");
    return {};
}
