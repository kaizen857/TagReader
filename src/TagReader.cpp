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
#include <cstring>
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

uint32_t ReadBE32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint32_t ReadBE24(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 16) |
           (static_cast<uint32_t>(data[1]) << 8) |
           static_cast<uint32_t>(data[2]);
}

uint32_t ReadBE16(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 8) |
           static_cast<uint32_t>(data[1]);
}

uint32_t ReadSyncSafe32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 21) |
           (static_cast<uint32_t>(data[1]) << 14) |
           (static_cast<uint32_t>(data[2]) << 7) |
           static_cast<uint32_t>(data[3]);
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size)
{
    std::vector<uint8_t> buffer(size);
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
    {
        return {};
    }

    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() != static_cast<std::streamsize>(buffer.size()))
    {
        return {};
    }

    return buffer;
}

std::string ReadLatin1Text(const uint8_t *data, std::size_t size)
{
    std::string value;
    value.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        if (data[i] == '\0')
        {
            break;
        }
        value.push_back(static_cast<char>(data[i]));
    }
    return TrimText(std::move(value));
}

std::string ReadUtf8Text(const uint8_t *data, std::size_t size)
{
    std::string value(reinterpret_cast<const char *>(data), size);
    const auto nul = value.find('\0');
    if (nul != std::string::npos)
    {
        value.resize(nul);
    }
    return TrimText(std::move(value));
}

std::string ReadUtf16Text(const uint8_t *data, std::size_t size, bool bigEndian)
{
    std::string value;
    value.reserve(size / 2);
    std::size_t start = 0;
    if (size >= 2)
    {
        if ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))
        {
            start = 2;
        }
    }

    for (std::size_t i = start; i + 1 < size; i += 2)
    {
        const uint16_t ch = bigEndian ? ReadBE16(data + i) : static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
        if (ch == 0)
        {
            break;
        }
        if (ch < 0x80)
        {
            value.push_back(static_cast<char>(ch));
        }
    }

    return TrimText(std::move(value));
}

std::string ReadId3TextFrame(const uint8_t *data, std::size_t size)
{
    if (size == 0)
    {
        return {};
    }

    const uint8_t encoding = data[0];
    const uint8_t *payload = data + 1;
    const std::size_t payloadSize = size - 1;

    switch (encoding)
    {
    case 0:
        return ReadLatin1Text(payload, payloadSize);
    case 1:
        return ReadUtf16Text(payload, payloadSize, false);
    case 2:
        return ReadUtf16Text(payload, payloadSize, true);
    case 3:
        return ReadUtf8Text(payload, payloadSize);
    default:
        return {};
    }
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

    // 这里直接从文件头解析 ID3v2，不依赖 FFmpeg 的通用 metadata 映射。
    const std::vector<uint8_t> header = ReadRange(context.input, 0, 10);
    if (header.size() != 10 || std::memcmp(header.data(), "ID3", 3) != 0)
    {
        return;
    }

    const uint8_t versionMajor = header[3];
    const uint8_t flags = header[5];
    const uint32_t tagSize = ReadSyncSafe32(header.data() + 6);
    const std::size_t tagEnd = static_cast<std::size_t>(10 + tagSize);

    std::size_t cursor = 10;
    if ((flags & 0x40) != 0 && tagEnd >= cursor + 4)
    {
        // 只做最小的扩展头跳过，不在这里展开更复杂的保护/加密逻辑。
        const std::vector<uint8_t> extHeader = ReadRange(context.input, cursor, 4);
        if (extHeader.size() == 4)
        {
            const uint32_t extSize = versionMajor >= 4 ? ReadSyncSafe32(extHeader.data()) : ReadBE32(extHeader.data());
            cursor += 4 + static_cast<std::size_t>(extSize);
        }
    }

    while (cursor + 10 <= tagEnd)
    {
        const std::vector<uint8_t> frameHeader = ReadRange(context.input, cursor, 10);
        if (frameHeader.size() != 10)
        {
            break;
        }

        if (frameHeader[0] == 0)
        {
            break;
        }

        const std::string frameId(reinterpret_cast<const char *>(frameHeader.data()), 4);
        const uint32_t frameSize = versionMajor >= 4 ? ReadSyncSafe32(frameHeader.data() + 4) : ReadBE32(frameHeader.data() + 4);
        if (frameSize == 0 || cursor + 10 + frameSize > tagEnd)
        {
            break;
        }

        const std::vector<uint8_t> frameData = ReadRange(context.input, cursor + 10, frameSize);
        if (frameData.size() != frameSize)
        {
            break;
        }

        ReadID3v2Frame(context, metadata, frameId, frameData.data(), frameData.size());

        cursor += 10 + static_cast<std::size_t>(frameSize);
    }
}

void TagReader::ReadID3v2Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameId == "APIC")
    {
        // APIC 帧单独走图片提取，不走文本帧路径。
        ReadID3v2PictureFrame(context, metadata, frameData, frameSize);
        return;
    }

    const std::string value = ReadId3TextFrame(frameData, frameSize);
    if (value.empty())
    {
        return;
    }

    if (frameId == "TIT2")
    {
        metadata.title = value;
    }
    else if (frameId == "TPE1")
    {
        metadata.artist = value;
    }
    else if (frameId == "TALB")
    {
        metadata.album = value;
    }
    else if (frameId == "TPE2")
    {
        metadata.albumArtist = value;
    }
    else if (frameId == "TCOM")
    {
        metadata.composer = value;
    }
    else if (frameId == "TCON")
    {
        metadata.genre = value;
    }
    else if (frameId == "TYER" || frameId == "TDRC")
    {
        metadata.year = ParseUInt16(value);
    }
    else if (frameId == "TRCK")
    {
        metadata.trackNumber = ParseSlashNumber(value).first;
    }
    else if (frameId == "TPOS")
    {
        metadata.discNumber = ParseSlashNumber(value).first;
    }
}

void TagReader::ReadID3v2PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameData == nullptr || frameSize < 4)
    {
        return;
    }

    (void)context;

    const uint8_t encoding = frameData[0];
    const uint8_t *payload = frameData + 1;
    const std::size_t payloadSize = frameSize - 1;

    std::size_t cursor = 0;
    while (cursor < payloadSize && payload[cursor] != 0)
    {
        ++cursor;
    }
    if (cursor >= payloadSize)
    {
        return;
    }

    const std::string mimeType = ReadLatin1Text(payload, cursor);
    ++cursor;
    if (cursor >= payloadSize)
    {
        return;
    }

    ++cursor;
    while (cursor < payloadSize && payload[cursor] != 0)
    {
        ++cursor;
    }
    ++cursor;
    if (cursor >= payloadSize)
    {
        return;
    }

    ReadID3v2ApicPayload(context, metadata, mimeType, payload + cursor, payloadSize - cursor);
    (void)encoding;
}

void TagReader::ReadID3v2ApicPayload(ReadContext &context, RawMetadata &metadata, std::string_view mimeType, const uint8_t *imageData, std::size_t imageSize)
{
    if (imageData == nullptr || imageSize == 0)
    {
        return;
    }

    std::string extension = "bin";
    if (mimeType.find("jpeg") != std::string_view::npos || mimeType.find("jpg") != std::string_view::npos)
    {
        extension = "jpg";
    }
    else if (mimeType.find("png") != std::string_view::npos)
    {
        extension = "png";
    }

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        throw std::runtime_error("failed to locate temp directory: " + ec.message());
    }

    const std::string tempPath = (tempDir / ("tagreader_cover_" + std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()) + "." + extension)).string();
    WriteBinaryFile(tempPath, imageData, imageSize);
    metadata.coverPath = tempPath;
}

void TagReader::ReadVorbisCommentMetadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    // 这里只做格式入口和最小扫描：按容器实际块内容读出 KEY=VALUE 记录。
    const std::string container = ToLower(context.containerName);
    std::string blob;

    if (container.find("flac") != std::string::npos)
    {
        ReadVorbisCommentBlock(context, metadata, 4, context.fileSize - 4);
        ReadFlacPictureBlock(context, metadata, 4, context.fileSize - 4);
    }
    else if (container.find("ogg") != std::string::npos || container.find("vorbis") != std::string::npos)
    {
        ReadOggVorbisComments(context, metadata);
    }
}

void TagReader::ReadOggVorbisComments(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    const std::vector<uint8_t> probe = ReadRange(context.input, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(context.fileSize, 4096)));
    if (probe.size() < 27 || std::string_view(reinterpret_cast<const char *>(probe.data()), 4) != "OggS")
    {
        return;
    }

    std::uintmax_t cursor = 0;
    while (cursor + 27 <= context.fileSize)
    {
        const std::vector<uint8_t> pageHeader = ReadRange(context.input, cursor, 27);
        if (pageHeader.size() != 27 || std::string_view(reinterpret_cast<const char *>(pageHeader.data()), 4) != "OggS")
        {
            return;
        }

        const uint8_t segmentCount = pageHeader[26];
        const std::vector<uint8_t> segmentTable = ReadRange(context.input, cursor + 27, segmentCount);
        if (segmentTable.size() != segmentCount)
        {
            return;
        }

        std::size_t payloadSize = 0;
        for (uint8_t seg : segmentTable)
        {
            payloadSize += seg;
        }

        const std::vector<uint8_t> payload = ReadRange(context.input, cursor + 27 + segmentCount, payloadSize);
        if (payload.size() != payloadSize)
        {
            return;
        }

        if (payload.size() > 7 && std::string_view(reinterpret_cast<const char *>(payload.data()), 7) == "vorbis")
        {
            std::size_t p = 7;
            if (p + 4 > payload.size()) return;
            const uint32_t vendorLen = ReadBE32(payload.data() + p); p += 4;
            if (p + vendorLen > payload.size()) return;
            p += vendorLen;
            if (p + 4 > payload.size()) return;
            const uint32_t commentCount = ReadBE32(payload.data() + p); p += 4;
            for (uint32_t i = 0; i < commentCount && p + 4 <= payload.size(); ++i)
            {
                const uint32_t len = ReadBE32(payload.data() + p); p += 4;
                if (p + len > payload.size()) return;
                ReadVorbisCommentEntry(metadata, std::string_view(reinterpret_cast<const char *>(payload.data() + p), len));
                p += len;
            }
            return;
        }

        cursor += 27 + segmentCount + payloadSize;
    }
}

void TagReader::ReadVorbisCommentBlock(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t size)
{
    if (!context.input.is_open() || size < 4)
    {
        return;
    }

    // 这里只处理 FLAC comment block 的最小实现，后续可复用到其他容器的 comment 区段。
    std::uintmax_t cursor = offset;
    while (cursor + 4 <= offset + size)
    {
        const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
        if (blockHeader.size() != 4)
        {
            return;
        }

        const bool lastBlock = (blockHeader[0] & 0x80) != 0;
        const uint32_t blockType = blockHeader[0] & 0x7F;
        const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
        cursor += 4;

        if (cursor + blockSize > offset + size)
        {
            return;
        }

        if (blockType == 4)
        {
            const std::vector<uint8_t> vendorLen = ReadRange(context.input, cursor, 4);
            if (vendorLen.size() != 4)
            {
                return;
            }

            const uint32_t vendorSize = ReadBE32(vendorLen.data());
            const std::vector<uint8_t> vendor = ReadRange(context.input, cursor + 4, vendorSize);
            if (vendor.size() != vendorSize)
            {
                return;
            }

            const std::vector<uint8_t> commentCountBuf = ReadRange(context.input, cursor + 4 + vendorSize, 4);
            if (commentCountBuf.size() != 4)
            {
                return;
            }

            const uint32_t commentCount = ReadBE32(commentCountBuf.data());
            std::size_t commentOffset = static_cast<std::size_t>(cursor + 4 + vendorSize + 4);
            for (uint32_t i = 0; i < commentCount; ++i)
            {
                const std::vector<uint8_t> lenBuf = ReadRange(context.input, commentOffset, 4);
                if (lenBuf.size() != 4)
                {
                    return;
                }

                const uint32_t len = ReadBE32(lenBuf.data());
                const std::vector<uint8_t> comment = ReadRange(context.input, commentOffset + 4, len);
                if (comment.size() != len)
                {
                    return;
                }

                ReadVorbisCommentEntry(metadata, std::string_view(reinterpret_cast<const char *>(comment.data()), comment.size()));
                commentOffset += 4 + len;
            }
        }

        cursor += blockSize;
        if (lastBlock)
        {
            break;
        }
    }
}

void TagReader::ReadVorbisCommentEntry(RawMetadata &metadata, std::string_view entry)
{
    const auto eq = entry.find('=');
    if (eq == std::string_view::npos)
    {
        return;
    }

    const std::string key = ToLower(std::string(entry.substr(0, eq)));
    const std::string value = TrimText(std::string(entry.substr(eq + 1)));
    if (value.empty())
    {
        return;
    }

    if (key == "title")
    {
        metadata.title = value;
    }
    else if (key == "artist")
    {
        metadata.artist = value;
    }
    else if (key == "album")
    {
        metadata.album = value;
    }
    else if (key == "albumartist")
    {
        metadata.albumArtist = value;
    }
    else if (key == "composer")
    {
        metadata.composer = value;
    }
    else if (key == "genre")
    {
        metadata.genre = value;
    }
    else if (key == "date" || key == "year")
    {
        metadata.year = metadata.year == 0 ? ParseUInt16(value) : metadata.year;
    }
    else if (key == "tracknumber")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (key == "discnumber")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void TagReader::ReadFlacPictureBlock(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t size)
{
    if (!context.input.is_open() || size < 32)
    {
        return;
    }

    // FLAC picture block 是独立的图片块，直接读出后落盘到临时目录。
    std::uintmax_t cursor = offset;
    while (cursor + 4 <= offset + size)
    {
        const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
        if (blockHeader.size() != 4)
        {
            return;
        }

        const bool lastBlock = (blockHeader[0] & 0x80) != 0;
        const uint32_t blockType = blockHeader[0] & 0x7F;
        const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
        cursor += 4;
        if (cursor + blockSize > offset + size)
        {
            return;
        }

        if (blockType == 6)
        {
            const std::vector<uint8_t> picture = ReadRange(context.input, cursor, blockSize);
            if (picture.size() == blockSize)
            {
                ReadFlacPictureEntry(context, metadata, picture.data(), picture.size());
            }
            return;
        }

        cursor += blockSize;
        if (lastBlock)
        {
            break;
        }
    }
}

void TagReader::ReadFlacPictureEntry(ReadContext &context, RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize)
{
    if (pictureData == nullptr || pictureSize < 32)
    {
        return;
    }

    std::size_t p = 0;
    auto need = [&](std::size_t n) { return p + n <= pictureSize; };
    auto skipU32 = [&]() {
        if (!need(4)) return false;
        p += 4;
        return true;
    };

    if (!skipU32()) return;
    if (!need(4)) return;
    const uint32_t mimeLen = ReadBE32(pictureData + p); p += 4;
    if (!need(mimeLen)) return;
    p += mimeLen;
    if (!skipU32()) return;
    if (!skipU32()) return;
    if (!skipU32()) return;
    if (!skipU32()) return;
    if (!need(4)) return;
    const uint32_t descLen = ReadBE32(pictureData + p); p += 4;
    if (!need(descLen)) return;
    p += descLen;
    if (!need(4)) return;
    const uint32_t picDataLen = ReadBE32(pictureData + p); p += 4;
    if (!need(picDataLen)) return;

    const std::string tempPath = MakeTempCoverPath();
    WriteBinaryFile(tempPath, pictureData + p, picDataLen);
    metadata.coverPath = tempPath;
}

void TagReader::ReadMP4Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    // MP4 / M4A 先扫 atom/box，定位到 `moov/udta/meta/ilst` 路径后再做字段映射。
    const std::string container = ToLower(context.containerName);
    if (container.find("mp4") == std::string::npos && container.find("mov") == std::string::npos && container.find("m4") == std::string::npos)
    {
        return;
    }

    // MP4 入口只负责开始 atom 递归，具体字段映射分散在更小的扫描逻辑里。
    ReadMP4AtomTree(context, metadata, 0, context.fileSize, 0);
}

void TagReader::ReadMP4AtomTree(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t limit, std::uint32_t depth)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    // 这里先实现 atom 树扫描骨架，后续可以继续把 `moov/udta/meta/ilst` 单独下钻出来。
    std::uintmax_t cursor = offset;
    while (cursor + 8 <= limit)
    {
        const std::vector<uint8_t> header = ReadRange(context.input, cursor, 8);
        if (header.size() != 8)
        {
            return;
        }

        uint64_t atomSize = ReadBE32(header.data());
        const std::string atomType(reinterpret_cast<const char *>(header.data() + 4), 4);

        if (atomSize == 1)
        {
            const std::vector<uint8_t> ext = ReadRange(context.input, cursor + 8, 8);
            if (ext.size() != 8)
            {
                return;
            }
            atomSize = (static_cast<uint64_t>(ReadBE32(ext.data())) << 32) | ReadBE32(ext.data() + 4);
        }

        if (atomSize < 8)
        {
            return;
        }

        const std::uintmax_t atomEnd = cursor + static_cast<std::uintmax_t>(atomSize);
        if (atomEnd > limit)
        {
            return;
        }

        if (atomType == "udta" || atomType == "meta" || atomType == "ilst" || atomType == "moov")
        {
            // 这些盒子后续要进一步细分，所以这里只递归进入更深层。
            const std::uintmax_t childOffset = atomType == "meta" ? cursor + 12 : cursor + 8;
            if (childOffset < atomEnd)
            {
                ReadMP4AtomTree(context, metadata, childOffset, atomEnd, depth + 1);
            }
        }

        if (atomType == "©nam" || atomType == "©ART" || atomType == "aART" || atomType == "©alb" || atomType == "©wrt" || atomType == "©gen" || atomType == "trkn" || atomType == "disk" || atomType == "covr")
        {
            ReadMP4ItemAtom(context, metadata, atomType, cursor + 8, atomEnd);
        }

        if (atomSize == 0)
        {
            return;
        }

        cursor = atomEnd;
    }
}

void TagReader::ReadMP4ItemAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit)
{
    if (!context.input.is_open() || offset + 8 > limit)
    {
        return;
    }

    const std::vector<uint8_t> header = ReadRange(context.input, offset, 8);
    if (header.size() != 8)
    {
        return;
    }

    uint64_t size = ReadBE32(header.data());
    std::string type(reinterpret_cast<const char *>(header.data() + 4), 4);
    std::uintmax_t payloadOffset = offset + 8;
    if (size == 1)
    {
        const std::vector<uint8_t> ext = ReadRange(context.input, offset + 8, 8);
        if (ext.size() != 8)
        {
            return;
        }
        size = (static_cast<uint64_t>(ReadBE32(ext.data())) << 32) | ReadBE32(ext.data() + 4);
        payloadOffset += 8;
    }

    if (size < 8 || offset + size > limit)
    {
        return;
    }

    if (type == "data")
    {
        const std::vector<uint8_t> data = ReadRange(context.input, payloadOffset, static_cast<std::size_t>(offset + size - payloadOffset));
        if (data.size() < 8)
        {
            return;
        }
        const uint32_t dataType = ReadBE32(data.data() + 0);
        ReadMP4DataAtom(context, metadata, atomType, dataType, data.data() + 8, data.size() - 8);
        return;
    }

    // 兼容 `ilst` 的子项：继续扫描内部 item atom。
    std::uintmax_t cursor = payloadOffset;
    while (cursor + 8 <= offset + size)
    {
        const std::vector<uint8_t> childHeader = ReadRange(context.input, cursor, 8);
        if (childHeader.size() != 8)
        {
            return;
        }

        uint64_t childSize = ReadBE32(childHeader.data());
        std::string childType(reinterpret_cast<const char *>(childHeader.data() + 4), 4);
        std::uintmax_t childPayloadOffset = cursor + 8;
        if (childSize == 1)
        {
            const std::vector<uint8_t> ext = ReadRange(context.input, cursor + 8, 8);
            if (ext.size() != 8)
            {
                return;
            }
            childSize = (static_cast<uint64_t>(ReadBE32(ext.data())) << 32) | ReadBE32(ext.data() + 4);
            childPayloadOffset += 8;
        }

        if (childSize < 8 || cursor + childSize > offset + size)
        {
            return;
        }

        if (childType == "data")
        {
            const std::vector<uint8_t> data = ReadRange(context.input, childPayloadOffset, static_cast<std::size_t>(cursor + childSize - childPayloadOffset));
            if (data.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(data.data());
                ReadMP4DataAtom(context, metadata, atomType, dataType, data.data() + 8, data.size() - 8);
            }
        }

        cursor += childSize;
    }
}

void TagReader::ReadMP4DataAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize)
{
    (void)context;
    if (payload == nullptr || payloadSize == 0)
    {
        return;
    }

    // MP4 的常见文本和数字字段在 data atom 内，这里只处理项目需要的固定字段。
    if (atomType == "©nam" || atomType == "©ART" || atomType == "aART" || atomType == "©alb" || atomType == "©wrt" || atomType == "©gen")
    {
        const std::string value = (dataType == 1 || dataType == 0) ? ReadUtf8Text(payload, payloadSize) : ReadUtf8Text(payload, payloadSize);
        if (value.empty())
        {
            return;
        }

        if (atomType == "©nam") metadata.title = value;
        else if (atomType == "©ART") metadata.artist = value;
        else if (atomType == "aART") metadata.albumArtist = value;
        else if (atomType == "©alb") metadata.album = value;
        else if (atomType == "©wrt") metadata.composer = value;
        else if (atomType == "©gen") metadata.genre = value;
        return;
    }

    if (atomType == "trkn" && payloadSize >= 8)
    {
        metadata.trackNumber = static_cast<uint16_t>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    }
    else if (atomType == "disk" && payloadSize >= 8)
    {
        metadata.discNumber = static_cast<uint16_t>((static_cast<uint16_t>(payload[2]) << 8) | payload[3]);
    }
    else if (atomType == "covr")
    {
        const std::string tempPath = MakeTempCoverPath();
        WriteBinaryFile(tempPath, payload, payloadSize);
        metadata.coverPath = tempPath;
    }
}

void TagReader::ExtractCoverToTempFile(ReadContext &context, RawMetadata &metadata)
{
    (void)context;
    (void)metadata;
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
