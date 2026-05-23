#include "TagReader.hpp"
#include "TagReaderInternal.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <system_error>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

#if defined(TAGREADER_HAS_ICONV)
#include <iconv.h>
#endif

void TagReader::ReadContext::FormatContextDeleter::operator()(AVFormatContext *context) const noexcept
{
    if (context != nullptr)
    {
        avformat_close_input(&context);
    }
}

namespace
{
using tagreader_internal::CoverDecodeLimits;
using tagreader_internal::Mp4AtomHeader;
using tagreader_internal::Mp4PathState;
using tagreader_internal::PendingMp4AtomRange;

// Resource limits
constexpr std::size_t kMaxGenericReadBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxId3TagBytes = 16z * 1024 * 1024;
constexpr std::size_t kMaxTextFieldBytes = 1z * 1024 * 1024;
constexpr std::size_t kMaxLyricsBytes = 8z * 1024 * 1024;
constexpr std::size_t kMaxDecodedTextBytes = 2z * 1024 * 1024;
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxOggPacketBytes = 8z * 1024 * 1024;
constexpr std::size_t kMaxOggScannedBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxOggPages = 100000;
constexpr std::size_t kMaxMp4AtomPayloadBytes = 64z * 1024 * 1024;
constexpr std::size_t kMaxMp4Atoms = 100000;

constexpr CoverDecodeLimits kCoverDecodeLimits{};

// General helpers

class MalformedTagError : public std::runtime_error
{
public:
    explicit MalformedTagError(const std::string &message) : std::runtime_error(message)
    {
    }
};

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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
    for (const auto needle : needles)
    {
        if (value.find(needle) != std::string_view::npos)
        {
            return true;
        }
    }
    return false;
}

uint32_t ReadBE16(const uint8_t *data);
std::string ReadLocaleEncodedText(const uint8_t *data, std::size_t size, std::string_view encoding);

bool IsMostlyPrintableText(std::string_view text)
{
    if (text.empty())
    {
        return false;
    }

    std::size_t printable = 0;
    std::size_t suspicious = 0;
    for (unsigned char ch : text)
    {
        if (ch == 0)
        {
            ++suspicious;
            continue;
        }

        if (ch >= 0x20 || ch == '\t' || ch == '\r' || ch == '\n')
        {
            ++printable;
            continue;
        }

        ++suspicious;
    }

    return printable > 0 && printable * 4 >= text.size() * 3 && suspicious * 5 <= text.size();
}

void RemoveUtf8Bom(std::string &value);

bool LooksLikeUtf16WithoutBom(std::string_view raw, bool bigEndian)
{
    if (raw.size() < 6 || (raw.size() % 2) != 0)
    {
        return false;
    }

    std::size_t nulOnHighByte = 0;
    std::size_t nulOnLowByte = 0;
    std::size_t asciiLikeUnits = 0;
    std::size_t suspiciousControls = 0;
    std::size_t units = 0;

    for (std::size_t i = 0; i + 1 < raw.size(); i += 2)
    {
        const unsigned char first = static_cast<unsigned char>(raw[i]);
        const unsigned char second = static_cast<unsigned char>(raw[i + 1]);
        const unsigned char high = bigEndian ? first : second;
        const unsigned char low = bigEndian ? second : first;
        const uint16_t codeUnit = bigEndian ? ReadBE16(reinterpret_cast<const uint8_t *>(raw.data() + i)) : static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8));
        ++units;

        if (high == 0)
        {
            ++nulOnHighByte;
        }
        if (low == 0)
        {
            ++nulOnLowByte;
        }

        if (codeUnit == 0)
        {
            break;
        }

        if (codeUnit >= 0x20 && codeUnit <= 0x7E)
        {
            ++asciiLikeUnits;
        }
        else if (codeUnit < 0x20 && codeUnit != '\t' && codeUnit != '\r' && codeUnit != '\n')
        {
            ++suspiciousControls;
        }
    }

    if (units < 3)
    {
        return false;
    }

    const std::size_t expectedNuls = bigEndian ? nulOnHighByte : nulOnLowByte;
    const std::size_t unexpectedNuls = bigEndian ? nulOnLowByte : nulOnHighByte;
    if (expectedNuls * 3 < units * 2)
    {
        return false;
    }
    if (unexpectedNuls * 4 > units)
    {
        return false;
    }
    if (asciiLikeUnits == 0)
    {
        return false;
    }

    return suspiciousControls * 4 <= units;
}

std::string DetectLegacyLocalEncoding(std::string_view raw)
{
#if defined(TAGREADER_HAS_ICONV)
    constexpr std::array<std::string_view, 8> candidates{
        "GB18030",
        "GBK",
        "SHIFT_JIS",
        "CP932",
        "BIG5",
        "WINDOWS-1252",
        "WINDOWS-1251",
        "WINDOWS-1250",
    };

    for (std::string_view candidate : candidates)
    {
        const std::string decoded = ReadLocaleEncodedText(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), candidate);
        if (!decoded.empty() && IsMostlyPrintableText(decoded))
        {
            return std::string(candidate);
        }
    }
#else
    (void)raw;
#endif

    return "latin-1";
}

std::string TrimText(std::string value)
{
    const auto isTrimChar = [](unsigned char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0';
    };

    const auto first = std::find_if_not(value.begin(), value.end(), isTrimChar);
    if (first == value.end())
    {
        return {};
    }

    const auto last = std::find_if_not(value.rbegin(), value.rend(), isTrimChar).base();
    return std::string(first, last);
}

// Cover cache helpers

std::string HexEncode(std::uint64_t value)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (std::size_t i = 0; i < hex.size(); ++i)
    {
        const std::size_t shift = (hex.size() - 1 - i) * 4;
        hex[i] = kHex[(value >> shift) & 0x0F];
    }

    return hex;
}

std::string HashEmbeddedImageBytes(const uint8_t *data, std::size_t size)
{
    constexpr std::uint64_t kFnvOffsetA = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrimeA = 1099511628211ULL;
    constexpr std::uint64_t kFnvOffsetB = 1099511628211ULL;
    constexpr std::uint64_t kFnvPrimeB = 14695981039346656037ULL;

    std::uint64_t hashA = kFnvOffsetA;
    std::uint64_t hashB = kFnvOffsetB ^ static_cast<std::uint64_t>(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::uint64_t byte = data == nullptr ? 0 : data[i];
        hashA ^= byte;
        hashA *= kFnvPrimeA;

        hashB ^= byte + static_cast<std::uint64_t>(i & 0xFF);
        hashB *= kFnvPrimeB;
    }

    return HexEncode(hashA) + HexEncode(hashB);
}

std::filesystem::path BuildCoverCachePath(const std::filesystem::path &coverExportDir, std::string_view hex)
{
    if (coverExportDir.empty() || hex.size() < 3)
    {
        return {};
    }

    return coverExportDir / std::string(hex.substr(0, 2)) / (std::string(hex.substr(2)) + ".png");
}

struct FileDescriptor
{
    int fd{-1};

    explicit FileDescriptor(int value) noexcept : fd(value)
    {
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    ~FileDescriptor()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    int get() const noexcept
    {
        return fd;
    }

    int release() noexcept
    {
        const int value = fd;
        fd = -1;
        return value;
    }
};

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path &finalPath)
{
    static std::atomic_uint64_t tempCounter{0};
    const auto now = std::filesystem::file_time_type::clock::now().time_since_epoch().count();
    const auto seq = tempCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string name = finalPath.filename().string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(now) + "." + std::to_string(seq);
    return finalPath.parent_path() / name;
}

void WriteAll(int fd, const uint8_t *data, std::size_t size, const std::filesystem::path &path)
{
    std::size_t written = 0;
    while (written < size)
    {
        const std::size_t remaining = size - written;
        const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write(fd, data + written, chunk);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error("failed to write cover file: " + path.string());
        }
        if (result == 0)
        {
            throw std::runtime_error("failed to write complete cover file: " + path.string());
        }
        written += static_cast<std::size_t>(result);
    }
}

bool AtomicWriteFileIfAbsent(const std::filesystem::path &finalPath, const uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        throw std::runtime_error("failed to create cover cache directory: " + ec.message());
    }

    if (std::filesystem::exists(finalPath, ec))
    {
        return true;
    }
    if (ec)
    {
        throw std::runtime_error("failed to query cover cache file: " + ec.message());
    }

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const std::filesystem::path tempPath = MakeSiblingTempPath(finalPath);
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
        FileDescriptor fd(::open(tempPath.c_str(), flags, 0600));
        if (fd.get() < 0)
        {
            if (errno == EEXIST)
            {
                continue;
            }
            throw std::runtime_error("failed to create temporary cover file: " + tempPath.string());
        }

        try
        {
            WriteAll(fd.get(), data, size, tempPath);
            if (::fsync(fd.get()) != 0)
            {
                throw std::runtime_error("failed to fsync cover file: " + tempPath.string());
            }
            if (::close(fd.release()) != 0)
            {
                throw std::runtime_error("failed to close cover file: " + tempPath.string());
            }
        }
        catch (...)
        {
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            throw;
        }

        if (::rename(tempPath.c_str(), finalPath.c_str()) == 0)
        {
            return true;
        }

        const int renameErrno = errno;
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        if (renameErrno == EEXIST || std::filesystem::exists(finalPath, ec))
        {
            return true;
        }
        throw std::runtime_error("failed to publish cover file: " + finalPath.string());
    }

    throw std::runtime_error("failed to create unique temporary cover file for: " + finalPath.string());
}

// Cover image decode helpers

enum class ImageFormat
{
    Unknown,
    Png,
    Jpeg,
    Bmp,
    Webp,
    Gif,
    Tiff,
};

ImageFormat DetectImageFormat(const uint8_t *data, std::size_t size)
{
    if (data == nullptr)
    {
        return ImageFormat::Unknown;
    }

    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
    {
        return ImageFormat::Png;
    }

    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
    {
        return ImageFormat::Jpeg;
    }

    if (size >= 2 && data[0] == 'B' && data[1] == 'M')
    {
        return ImageFormat::Bmp;
    }

    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
    {
        return ImageFormat::Webp;
    }

    if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
    {
        return ImageFormat::Gif;
    }

    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) || (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A)))
    {
        return ImageFormat::Tiff;
    }

    return ImageFormat::Unknown;
}

struct AvFrameDeleter
{
    void operator()(AVFrame *frame) const noexcept
    {
        av_frame_free(&frame);
    }
};

struct AvPacketDeleter
{
    void operator()(AVPacket *packet) const noexcept
    {
        av_packet_free(&packet);
    }
};

struct AvCodecContextDeleter
{
    void operator()(AVCodecContext *context) const noexcept
    {
        avcodec_free_context(&context);
    }
};

struct SwsContextDeleter
{
    void operator()(SwsContext *context) const noexcept
    {
        sws_freeContext(context);
    }
};

std::vector<uint8_t> ReadImageBytes(const uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0 || size > kCoverDecodeLimits.maxInputBytes || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr)
    {
        return {};
    }

    if (av_new_packet(packet.get(), static_cast<int>(size)) < 0)
    {
        return {};
    }
    std::memcpy(packet->data, data, size);
    return std::vector<uint8_t>(packet->data, packet->data + packet->size);
}

bool DecodedFrameWithinCoverLimits(const AVFrame *frame)
{
    if (frame == nullptr || frame->width <= 0 || frame->height <= 0)
    {
        return false;
    }
    if (frame->width > kCoverDecodeLimits.maxWidth || frame->height > kCoverDecodeLimits.maxHeight)
    {
        return false;
    }
    if (av_image_check_size(frame->width, frame->height, 0, nullptr) < 0)
    {
        return false;
    }

    const auto width = static_cast<std::int64_t>(frame->width);
    const auto height = static_cast<std::int64_t>(frame->height);
    if (width > std::numeric_limits<std::int64_t>::max() / height)
    {
        return false;
    }

    return width * height <= kCoverDecodeLimits.maxPixels;
}

std::vector<uint8_t> EncodeFrameAsPng(const AVFrame *frame)
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (encoder == nullptr)
    {
        return {};
    }

    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> encoderContext(avcodec_alloc_context3(encoder));
    if (encoderContext == nullptr)
    {
        return {};
    }

    encoderContext->width = frame->width;
    encoderContext->height = frame->height;
    encoderContext->pix_fmt = AV_PIX_FMT_RGB24;
    encoderContext->time_base = AVRational{1, 1};
    if (avcodec_open2(encoderContext.get(), encoder, nullptr) < 0)
    {
        return {};
    }

    if (avcodec_send_frame(encoderContext.get(), frame) < 0)
    {
        return {};
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr)
    {
        return {};
    }

    if (avcodec_receive_packet(encoderContext.get(), packet.get()) < 0)
    {
        return {};
    }

    return std::vector<uint8_t>(packet->data, packet->data + packet->size);
}

std::vector<uint8_t> ConvertImageToPng(const uint8_t *data, std::size_t size, AVCodecID codecId)
{
    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (decoder == nullptr)
    {
        return {};
    }

    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> decoderContext(avcodec_alloc_context3(decoder));
    if (decoderContext == nullptr || avcodec_open2(decoderContext.get(), decoder, nullptr) < 0)
    {
        return {};
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> decodedFrame(av_frame_alloc());
    if (decodedFrame == nullptr)
    {
        return {};
    }

    const std::vector<uint8_t> packetBytes = ReadImageBytes(data, size);
    if (packetBytes.empty())
    {
        return {};
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr)
    {
        return {};
    }
    if (av_new_packet(packet.get(), static_cast<int>(packetBytes.size())) < 0)
    {
        return {};
    }
    std::memcpy(packet->data, packetBytes.data(), packetBytes.size());

    if (avcodec_send_packet(decoderContext.get(), packet.get()) < 0 || avcodec_receive_frame(decoderContext.get(), decodedFrame.get()) < 0)
    {
        return {};
    }

    if (!DecodedFrameWithinCoverLimits(decodedFrame.get()))
    {
        return {};
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> rgbFrame(av_frame_alloc());
    if (rgbFrame == nullptr)
    {
        return {};
    }
    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = decodedFrame->width;
    rgbFrame->height = decodedFrame->height;
    if (av_frame_get_buffer(rgbFrame.get(), 1) < 0)
    {
        return {};
    }

    std::unique_ptr<SwsContext, SwsContextDeleter> swsContext(sws_getContext(decodedFrame->width,
                                                                             decodedFrame->height,
                                                                             static_cast<AVPixelFormat>(decodedFrame->format),
                                                                             rgbFrame->width,
                                                                             rgbFrame->height,
                                                                             AV_PIX_FMT_RGB24,
                                                                             SWS_BILINEAR,
                                                                             nullptr,
                                                                             nullptr,
                                                                             nullptr));
    if (swsContext == nullptr)
    {
        return {};
    }

    sws_scale(swsContext.get(), decodedFrame->data, decodedFrame->linesize, 0, decodedFrame->height, rgbFrame->data, rgbFrame->linesize);
    std::vector<uint8_t> png = EncodeFrameAsPng(rgbFrame.get());
    if (png.size() > kCoverDecodeLimits.maxOutputBytes)
    {
        return {};
    }

    return png;
}

std::vector<uint8_t> ConvertJpegToPng(const uint8_t *data, std::size_t size)
{
    return ConvertImageToPng(data, size, AV_CODEC_ID_MJPEG);
}

std::vector<uint8_t> DecodeAndEncodeCoverPng(const uint8_t *data, std::size_t size)
{
    const ImageFormat format = DetectImageFormat(data, size);
    std::vector<uint8_t> png;
    if (format == ImageFormat::Png)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_PNG);
    }
    else if (format == ImageFormat::Jpeg)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_MJPEG);
    }
    else if (format == ImageFormat::Bmp)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_BMP);
    }
    else if (format == ImageFormat::Webp)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_WEBP);
    }
    else if (format == ImageFormat::Gif)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_GIF);
    }
    else if (format == ImageFormat::Tiff)
    {
        png = ConvertImageToPng(data, size, AV_CODEC_ID_TIFF);
    }
    else
    {
        constexpr std::array<AVCodecID, 6> fallbackCodecs{
            AV_CODEC_ID_PNG,
            AV_CODEC_ID_WEBP,
            AV_CODEC_ID_GIF,
            AV_CODEC_ID_TIFF,
            AV_CODEC_ID_BMP,
            AV_CODEC_ID_MJPEG,
        };

        for (AVCodecID codecId : fallbackCodecs)
        {
            png = ConvertImageToPng(data, size, codecId);
            if (!png.empty())
            {
                break;
            }
        }
    }

    if (png.empty() || png.size() > kCoverDecodeLimits.maxOutputBytes || DetectImageFormat(png.data(), png.size()) != ImageFormat::Png)
    {
        return {};
    }

    return png;
}

std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size)
{
    if (coverExportDir.empty() || data == nullptr || size == 0 || size > kMaxCoverInputBytes)
    {
        return {};
    }

    // All public cover exports use PNG paths; JPEG sources must be transcoded, not renamed.
    const std::filesystem::path coverPath = BuildCoverCachePath(coverExportDir, HashEmbeddedImageBytes(data, size));
    std::error_code existsEc;
    if (std::filesystem::exists(coverPath, existsEc))
    {
        return coverPath;
    }
    if (existsEc)
    {
        throw std::runtime_error("failed to query cover cache file: " + existsEc.message());
    }

    std::error_code ec;
    std::filesystem::create_directories(coverPath.parent_path(), ec);
    if (ec)
    {
        throw std::runtime_error("failed to create cover cache shard directory: " + ec.message());
    }
    if (ToLower(coverPath.extension().string()) != ".png")
    {
        throw std::runtime_error("cover export path must use .png extension");
    }

    std::vector<uint8_t> png = DecodeAndEncodeCoverPng(data, size);
    if (png.empty())
    {
        return {};
    }

    if (!AtomicWriteFileIfAbsent(coverPath, png.data(), png.size()))
    {
        return {};
    }
    return coverPath;
}
} // namespace

MusicTag TagReader::Read(const std::filesystem::path &filePath)
{
    return Read(filePath, {});
}

MusicTag TagReader::Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir)
{
    // FFmpeg 的全局日志保持静默，避免测试程序污染终端输出。
    av_log_set_level(AV_LOG_QUIET);
#if LIBAVFORMAT_VERSION_MAJOR < 59
    av_register_all();
#endif

    ValidatePath(filePath);

    ReadContext context = OpenContext(filePath);
    context.coverExportDir = coverExportDir;
    ValidateCoverExportDir(context.coverExportDir);
    DetectStream(context);
    context.detectedContainer = DetectContainer(context);

    const RawMediaInfo mediaInfo = ReadMediaInfo(context);
    const RawMetadata metadata = ReadMetadata(context);
    const RawLyrics lyrics = ReadLyrics(context);

    return BuildMusicTag(context, mediaInfo, metadata, lyrics);
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
    constexpr auto readMask = std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
    if ((perms & readMask) == std::filesystem::perms::none)
    {
        throw std::runtime_error("file is not readable: " + filePath.string());
    }
}

void TagReader::ValidateCoverExportDir(const std::filesystem::path &coverExportDir)
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

    const int bestAudioStream = av_find_best_stream(context.formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (bestAudioStream >= 0)
    {
        context.audioStreamIndex = bestAudioStream;
    }
    else
    {
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
        if (parsed > std::numeric_limits<uint16_t>::max())
        {
            return 0;
        }
        return static_cast<uint16_t>(parsed);
    }
    catch (...)
    {
        return 0;
    }
}

bool ParseDecimalU16Strict(std::string_view text, uint16_t &value)
{
    if (text.empty())
    {
        return false;
    }

    uint32_t result = 0;
    for (char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        result = result * 10 + static_cast<uint32_t>(ch - '0');
        if (result > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }
    }

    value = static_cast<uint16_t>(result);
    return true;
}

uint16_t ParseYearOnly(std::string_view text)
{
    while (!text.empty())
    {
        const unsigned char ch = static_cast<unsigned char>(text.front());
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        {
            text.remove_prefix(1);
            continue;
        }
        break;
    }

    if (text.size() < 4)
    {
        return 0;
    }

    if (!std::isdigit(static_cast<unsigned char>(text[0])) || !std::isdigit(static_cast<unsigned char>(text[1])) || !std::isdigit(static_cast<unsigned char>(text[2])) || !std::isdigit(static_cast<unsigned char>(text[3])))
    {
        return 0;
    }

    if (text.size() > 4)
    {
        const unsigned char next = static_cast<unsigned char>(text[4]);
        if (std::isdigit(next))
        {
            return 0;
        }

        const bool allowedSeparator = next == '-' || next == '/' || next == '.' || next == ' ' || next == 'T' || next == '\0';
        if (!allowedSeparator)
        {
            return 0;
        }
    }

    const uint16_t year = static_cast<uint16_t>((text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0'));
    return (year >= 1000 && year <= 9999) ? year : 0;
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

// Byte readers and bounded range helpers

uint32_t ReadBE32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t ReadBE24(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | static_cast<uint32_t>(data[2]);
}

uint32_t ReadBE16(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 8) | static_cast<uint32_t>(data[1]);
}

uint32_t ReadLE32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t ReadSyncSafe32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 21) | (static_cast<uint32_t>(data[1]) << 14) | (static_cast<uint32_t>(data[2]) << 7) | static_cast<uint32_t>(data[3]);
}

bool IsValidSyncSafe32(const uint8_t *data)
{
    return (data[0] & 0x80) == 0 && (data[1] & 0x80) == 0 && (data[2] & 0x80) == 0 && (data[3] & 0x80) == 0;
}

bool TryAddUintmax(std::uintmax_t base, std::uintmax_t delta, std::uintmax_t &result)
{
    if (base > std::numeric_limits<std::uintmax_t>::max() - delta)
    {
        return false;
    }
    result = base + delta;
    return true;
}

bool TryAddSize(std::size_t base, std::size_t delta, std::size_t &result)
{
    if (base > std::numeric_limits<std::size_t>::max() - delta)
    {
        return false;
    }
    result = base + delta;
    return true;
}

uint32_t ClampToUint32(std::int64_t value)
{
    if (value < 0 || value > std::numeric_limits<uint32_t>::max())
    {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

uint8_t ClampToUint8(int value)
{
    if (value < 0 || value > std::numeric_limits<uint8_t>::max())
    {
        return 0;
    }
    return static_cast<uint8_t>(value);
}

uint32_t DetectAudioBitDepth(const AVCodecParameters *codecpar)
{
    if (codecpar == nullptr)
    {
        return 0;
    }

    if (codecpar->bits_per_raw_sample > 0)
    {
        return ClampToUint32(codecpar->bits_per_raw_sample);
    }

    if (codecpar->bits_per_coded_sample > 0)
    {
        return ClampToUint32(codecpar->bits_per_coded_sample);
    }

    return 0;
}

std::string NormalizeFormatName(const std::filesystem::path &filePath, std::string_view containerName)
{
    std::string normalized = ToLower(std::string(containerName));
    if (normalized.empty())
    {
        return normalized;
    }

    if (normalized.find(',') == std::string::npos)
    {
        return normalized;
    }

    std::string extension = ToLower(filePath.extension().string());
    if (!extension.empty() && extension.front() == '.')
    {
        extension.erase(extension.begin());
    }

    if (!extension.empty())
    {
        return extension;
    }

    const auto comma = normalized.find(',');
    return comma == std::string::npos ? normalized : normalized.substr(0, comma);
}

// Ogg/Vorbis comment helpers

template <typename Handler>
bool ForEachVorbisCommentEntry(const uint8_t *data, std::size_t size, Handler &&handler)
{
    if (data == nullptr || size < 8)
    {
        return false;
    }

    std::size_t cursor = 0;
    const uint32_t vendorLength = ReadLE32(data + cursor);
    cursor += 4;
    if (vendorLength > size - cursor)
    {
        return false;
    }
    cursor += vendorLength;

    if (size - cursor < 4)
    {
        return false;
    }

    const uint32_t commentCount = ReadLE32(data + cursor);
    cursor += 4;
    for (uint32_t i = 0; i < commentCount; ++i)
    {
        if (size - cursor < 4)
        {
            return false;
        }

        const uint32_t commentLength = ReadLE32(data + cursor);
        cursor += 4;
        if (commentLength > size - cursor)
        {
            return false;
        }

        handler(std::string_view(reinterpret_cast<const char *>(data + cursor), commentLength));
        cursor += commentLength;
    }

    return true;
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size, std::size_t maxSize)
{
    if (size > maxSize)
    {
        input.clear();
        return {};
    }
    if (offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
    {
        input.clear();
        return {};
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        input.clear();
        return {};
    }
    if (offset > std::numeric_limits<std::uintmax_t>::max() - static_cast<std::uintmax_t>(size))
    {
        input.clear();
        return {};
    }

    std::vector<uint8_t> buffer(size);
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
    {
        input.clear();
        return {};
    }

    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() != static_cast<std::streamsize>(buffer.size()))
    {
        input.clear();
        return {};
    }

    return buffer;
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size)
{
    return ReadRange(input, offset, size, kMaxGenericReadBytes);
}

bool IsValidUtf8(std::string_view text)
{
    const auto *ptr = reinterpret_cast<const unsigned char *>(text.data());
    std::size_t i = 0;
    while (i < text.size())
    {
        const unsigned char c = ptr[i];
        if (c <= 0x7F)
        {
            ++i;
            continue;
        }

        uint32_t codePoint = 0;
        std::size_t need = 0;
        if ((c & 0xE0) == 0xC0)
        {
            codePoint = c & 0x1F;
            need = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            codePoint = c & 0x0F;
            need = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            codePoint = c & 0x07;
            need = 3;
        }
        else
        {
            return false;
        }

        if (i + need >= text.size())
        {
            return false;
        }

        for (std::size_t j = 1; j <= need; ++j)
        {
            const unsigned char tail = ptr[i + j];
            if ((tail & 0xC0) != 0x80)
            {
                return false;
            }
            codePoint = (codePoint << 6) | (tail & 0x3F);
        }

        if ((need == 1 && codePoint < 0x80) || (need == 2 && codePoint < 0x800) || (need == 3 && codePoint < 0x10000) || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            return false;
        }

        i += need + 1;
    }

    return true;
}

bool IsLikelyId3FrameId(std::string_view frameId)
{
    if (frameId.size() != 4)
    {
        return false;
    }

    return std::all_of(frameId.begin(), frameId.end(), [](unsigned char ch)
                       { return std::isalnum(ch) != 0 || ch == '_'; });
}

bool IsLikelyId3v22FrameId(std::string_view frameId)
{
    if (frameId.size() != 3)
    {
        return false;
    }

    return std::all_of(frameId.begin(), frameId.end(), [](unsigned char ch)
                       { return std::isalnum(ch) != 0 || ch == '_'; });
}

std::size_t FindEncodedTerminator(const uint8_t *data, std::size_t size, uint8_t encoding)
{
    if (encoding == 1 || encoding == 2)
    {
        for (std::size_t i = 0; i + 1 < size; i += 2)
        {
            if (data[i] == 0 && data[i + 1] == 0)
            {
                return i;
            }
        }
        return size;
    }

    for (std::size_t i = 0; i < size; ++i)
    {
        if (data[i] == 0)
        {
            return i;
        }
    }
    return size;
}

std::size_t EncodedTerminatorWidth(uint8_t encoding)
{
    return (encoding == 1 || encoding == 2) ? 2U : 1U;
}

std::vector<uint8_t> RemoveId3Unsynchronization(std::vector<uint8_t> bytes)
{
    std::vector<uint8_t> result;
    result.reserve(bytes.size());

    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        result.push_back(bytes[i]);
        if (bytes[i] == 0xFF && i + 1 < bytes.size() && bytes[i + 1] == 0x00)
        {
            ++i;
        }
    }

    return result;
}

bool PrepareId3v23Or24FrameData(uint8_t versionMajor, uint16_t frameFlags, std::vector<uint8_t> &frameData)
{
    std::size_t payloadCursor = 0;
    bool hasFrameUnsynchronization = false;
    bool hasDataLengthIndicator = false;
    uint32_t declaredSize = 0;

    if (versionMajor == 3)
    {
        const bool hasCompression = (frameFlags & 0x0080) != 0;
        const bool hasEncryption = (frameFlags & 0x0040) != 0;
        const bool hasGroupingIdentity = (frameFlags & 0x0020) != 0;
        if (hasCompression || hasEncryption)
        {
            return false;
        }
        if (hasGroupingIdentity)
        {
            if (frameData.size() < 1)
            {
                return false;
            }
            payloadCursor += 1;
        }
    }
    else
    {
        const bool hasGroupingIdentity = (frameFlags & 0x0040) != 0;
        const bool hasCompression = (frameFlags & 0x0008) != 0;
        const bool hasEncryption = (frameFlags & 0x0004) != 0;
        hasFrameUnsynchronization = (frameFlags & 0x0002) != 0;
        hasDataLengthIndicator = (frameFlags & 0x0001) != 0;
        if (hasCompression || hasEncryption)
        {
            return false;
        }
        if (hasGroupingIdentity)
        {
            if (frameData.size() < 1)
            {
                return false;
            }
            payloadCursor += 1;
        }
        if (hasDataLengthIndicator)
        {
            if (frameData.size() - payloadCursor < 4 || !IsValidSyncSafe32(frameData.data() + payloadCursor))
            {
                return false;
            }

            declaredSize = ReadSyncSafe32(frameData.data() + payloadCursor);
            payloadCursor += 4;
            if (declaredSize > frameData.size() - payloadCursor)
            {
                return false;
            }
        }
    }

    if (payloadCursor > 0)
    {
        frameData.erase(frameData.begin(), frameData.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
    }

    if (hasDataLengthIndicator)
    {
        frameData.resize(declaredSize);
    }

    if (versionMajor == 4 && hasFrameUnsynchronization)
    {
        frameData = RemoveId3Unsynchronization(std::move(frameData));
    }

    return true;
}

bool PrepareId3v24FrameRegion(const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, uint8_t tagFlags, std::size_t &cursor, std::size_t &frameLimit)
{
    frameLimit = tagBytes.size();
    if (versionMajor == 4 && (tagFlags & 0x10) != 0)
    {
        // ID3v2.4 footer is part of tag size but must never be scanned as a frame.
        if (frameLimit < 10)
        {
            return false;
        }

        const std::size_t footerOffset = frameLimit - 10;
        const uint8_t *footer = tagBytes.data() + footerOffset;
        if (std::memcmp(footer, "3DI", 3) != 0 || footer[3] != 4 || !IsValidSyncSafe32(footer + 6))
        {
            return false;
        }
        frameLimit = footerOffset;
    }

    if (versionMajor == 4 && (tagFlags & 0x40) != 0)
    {
        if (frameLimit < 6 || !IsValidSyncSafe32(tagBytes.data()))
        {
            return false;
        }

        const std::size_t extSize = ReadSyncSafe32(tagBytes.data());
        std::size_t extendedEnd = 0;
        if (extSize < 6 || !TryAddSize(0, extSize, extendedEnd) || extendedEnd > frameLimit)
        {
            return false;
        }

        const uint8_t flagBytes = tagBytes[4];
        std::size_t flagEnd = 0;
        if (flagBytes == 0 || !TryAddSize(5, flagBytes, flagEnd) || flagEnd > extSize)
        {
            return false;
        }

        cursor = extendedEnd;
    }

    return true;
}

bool PrepareId3v23ExtendedHeader(const std::vector<uint8_t> &tagBytes, std::size_t frameLimit, std::size_t &cursor)
{
    if (frameLimit < 4)
    {
        return false;
    }

    const std::size_t extSize = ReadBE32(tagBytes.data());
    std::size_t extendedEnd = 0;
    if (extSize < 6 || !TryAddSize(4, extSize, extendedEnd) || extendedEnd > frameLimit)
    {
        return false;
    }

    cursor = extendedEnd;
    return true;
}

std::string ReadLatin1Text(const uint8_t *data, std::size_t size)
{
    if (data == nullptr)
    {
        return {};
    }

    std::string value;
    value.reserve(std::min(kMaxDecodedTextBytes, size));
    for (std::size_t i = 0; i < size; ++i)
    {
        const unsigned char ch = data[i];
        if (ch == 0)
        {
            break;
        }

        if (ch < 0x80)
        {
            if (value.size() >= kMaxDecodedTextBytes)
            {
                return {};
            }
            value.push_back(static_cast<char>(ch));
        }
        else
        {
            if (value.size() > kMaxDecodedTextBytes - 2)
            {
                return {};
            }
            value.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            value.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return TrimText(std::move(value));
}

#if defined(TAGREADER_HAS_ICONV)
std::string ConvertTextWithIconv(const uint8_t *data, std::size_t size, const char *encoding)
{
    if (data == nullptr || size == 0 || encoding == nullptr || *encoding == '\0')
    {
        return {};
    }

    iconv_t cd = iconv_open("UTF-8", encoding);
    if (cd == reinterpret_cast<iconv_t>(-1))
    {
        return {};
    }

    if (size > kMaxDecodedTextBytes / 4)
    {
        return {};
    }

    std::string output(std::min(kMaxDecodedTextBytes, std::max<std::size_t>(size * 4, 64)), '\0');
    const char *inputData = reinterpret_cast<const char *>(data);
    std::size_t inputLeft = size;

    char *outputData = output.data();
    std::size_t outputLeft = output.size();

    while (inputLeft > 0)
    {
        const std::size_t result = iconv(cd, const_cast<char **>(&inputData), &inputLeft, &outputData, &outputLeft);
        if (result != static_cast<std::size_t>(-1))
        {
            continue;
        }

        if (errno == E2BIG)
        {
            const std::size_t used = output.size() - outputLeft;
            if (output.size() > kMaxDecodedTextBytes / 2)
            {
                iconv_close(cd);
                return {};
            }
            const std::size_t nextSize = output.size() * 2;
            if (nextSize > kMaxDecodedTextBytes)
            {
                iconv_close(cd);
                return {};
            }
            output.resize(nextSize, '\0');
            outputData = output.data() + used;
            outputLeft = output.size() - used;
            continue;
        }

        iconv_close(cd);
        return {};
    }

    iconv_close(cd);
    output.resize(output.size() - outputLeft);
    RemoveUtf8Bom(output);
    output = TrimText(std::move(output));
    return IsValidUtf8(output) ? output : std::string{};
}
#endif

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

bool TryReadUtf16Text(const uint8_t *data, std::size_t size, bool defaultBigEndian, std::string &value)
{
    value.clear();
    if (data == nullptr)
    {
        return false;
    }

    value.reserve(size);
    std::size_t start = 0;
    bool bigEndian = defaultBigEndian;
    if (size >= 2)
    {
        if (data[0] == 0xFE && data[1] == 0xFF)
        {
            start = 2;
            bigEndian = true;
        }
        else if (data[0] == 0xFF && data[1] == 0xFE)
        {
            start = 2;
            bigEndian = false;
        }
    }

    if ((size - start) % 2 != 0)
    {
        return false;
    }

    for (std::size_t i = start; i + 1 < size; i += 2)
    {
        const uint16_t ch = bigEndian ? ReadBE16(data + i) : static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
        if (ch == 0)
        {
            break;
        }

        if (ch >= 0xD800 && ch <= 0xDBFF)
        {
            if (i + 3 >= size)
            {
                value.clear();
                return false;
            }

            const uint16_t low = bigEndian ? ReadBE16(data + i + 2) : static_cast<uint16_t>(data[i + 2] | (static_cast<uint16_t>(data[i + 3]) << 8));
            if (low < 0xDC00 || low > 0xDFFF)
            {
                value.clear();
                return false;
            }

            const uint32_t codePoint = 0x10000 + (((static_cast<uint32_t>(ch) - 0xD800) << 10) | (static_cast<uint32_t>(low) - 0xDC00));
            value.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
            value.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            i += 2;
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF)
        {
            value.clear();
            return false;
        }

        if (ch < 0x80)
        {
            value.push_back(static_cast<char>(ch));
        }
        else if (ch < 0x800)
        {
            value.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            value.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
        else
        {
            value.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            value.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }

    value = TrimText(std::move(value));
    return true;
}

void RemoveUtf8Bom(std::string &value)
{
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF && static_cast<unsigned char>(value[1]) == 0xBB && static_cast<unsigned char>(value[2]) == 0xBF)
    {
        value.erase(0, 3);
    }
}

std::string ReadUtf16Text(const uint8_t *data, std::size_t size, bool bigEndian)
{
    std::string value;
    if (!TryReadUtf16Text(data, size, bigEndian, value))
    {
        return {};
    }
    return value;
}

std::string ReadUtf16TextWithBom(const uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return {};
    }

    if (size >= 2)
    {
        if (data[0] == 0xFE && data[1] == 0xFF)
        {
            return ReadUtf16Text(data, size, true);
        }
        if (data[0] == 0xFF && data[1] == 0xFE)
        {
            return ReadUtf16Text(data, size, false);
        }
    }

    return ReadUtf16Text(data, size, false);
}

std::string ReadLocaleEncodedText(const uint8_t *data, std::size_t size, std::string_view encoding)
{
    if (data == nullptr || size == 0)
    {
        return {};
    }

#if defined(TAGREADER_HAS_ICONV)
    std::string encodingName(encoding);
    std::string decoded = ConvertTextWithIconv(data, size, encodingName.c_str());
    if (!decoded.empty())
    {
        return decoded;
    }
#else
    (void)encoding;
#endif

    return {};
}

std::string ReadId3ByteString(const uint8_t *data, std::size_t size, uint8_t encoding)
{
    switch (encoding)
    {
    case 0:
        return ReadLatin1Text(data, size);
    case 1:
        return ReadUtf16TextWithBom(data, size);
    case 2:
        return ReadUtf16Text(data, size, true);
    case 3:
    {
        const std::string utf8 = ReadUtf8Text(data, size);
        return IsValidUtf8(utf8) ? utf8 : std::string{};
    }
    default:
        return {};
    }
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

    return ReadId3ByteString(payload, payloadSize, encoding);
}

bool AtomTypeIs(std::string_view atomType, std::string_view expected)
{
    return atomType.size() == expected.size() && std::memcmp(atomType.data(), expected.data(), expected.size()) == 0;
}

// MP4 atom helpers

bool ReadMp4AtomHeader(std::ifstream &input, std::uintmax_t offset, std::uintmax_t limit, bool allowSizeZero, Mp4AtomHeader &atom)
{
    atom = {};
    if (limit <= offset)
    {
        return false;
    }

    std::uintmax_t basicHeaderEnd = 0;
    if (!TryAddUintmax(offset, 8, basicHeaderEnd) || basicHeaderEnd > limit)
    {
        return false;
    }

    const std::vector<uint8_t> header = ReadRange(input, offset, 8);
    if (header.size() != 8)
    {
        return false;
    }

    uint64_t atomSize = ReadBE32(header.data());
    std::string atomType(reinterpret_cast<const char *>(header.data() + 4), 4);
    std::uintmax_t headerSize = 8;
    std::uintmax_t payloadOffset = basicHeaderEnd;

    if (atomSize == 1)
    {
        std::uintmax_t extHeaderEnd = 0;
        if (!TryAddUintmax(offset, 16, extHeaderEnd) || extHeaderEnd > limit)
        {
            return false;
        }

        const std::vector<uint8_t> ext = ReadRange(input, basicHeaderEnd, 8);
        if (ext.size() != 8)
        {
            return false;
        }

        atomSize = (static_cast<uint64_t>(ReadBE32(ext.data())) << 32) | ReadBE32(ext.data() + 4);
        headerSize = 16;
        payloadOffset = extHeaderEnd;
    }

    std::uintmax_t atomEnd = limit;
    if (atomSize == 0)
    {
        if (!allowSizeZero)
        {
            return false;
        }
    }
    else
    {
        if (atomSize < headerSize)
        {
            return false;
        }
        if (atomSize > std::numeric_limits<std::uintmax_t>::max())
        {
            return false;
        }
        if (!TryAddUintmax(offset, static_cast<std::uintmax_t>(atomSize), atomEnd) || atomEnd > limit)
        {
            return false;
        }
    }

    atom.offset = offset;
    atom.headerSize = headerSize;
    atom.payloadOffset = payloadOffset;
    atom.atomEnd = atomEnd;
    atom.atomSize = atomSize;
    atom.atomType = std::move(atomType);
    return true;
}

constexpr std::array<char, 4> kMp4TitleAtom{static_cast<char>(0xA9), 'n', 'a', 'm'};
constexpr std::array<char, 4> kMp4ArtistAtom{static_cast<char>(0xA9), 'A', 'R', 'T'};
constexpr std::array<char, 4> kMp4AlbumAtom{static_cast<char>(0xA9), 'a', 'l', 'b'};
constexpr std::array<char, 4> kMp4ComposerAtom{static_cast<char>(0xA9), 'w', 'r', 't'};
constexpr std::array<char, 4> kMp4GenreAtom{static_cast<char>(0xA9), 'g', 'e', 'n'};
constexpr std::array<char, 4> kMp4DayAtom{static_cast<char>(0xA9), 'd', 'a', 'y'};
constexpr std::array<char, 4> kMp4DateAtom{'d', 'a', 't', 'e'};
constexpr std::array<char, 4> kMp4LyricsAtom{static_cast<char>(0xA9), 'l', 'y', 'r'};

std::size_t Mp4MetadataPayloadLimit(std::string_view atomType)
{
    if (atomType == "covr")
    {
        return kMaxCoverInputBytes;
    }
    if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) || AtomTypeIs(atomType, "aART") || AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
    {
        return kMaxTextFieldBytes;
    }

    return kMaxMp4AtomPayloadBytes;
}

std::vector<uint8_t> ReadMp4AtomPayload(std::ifstream &input, const Mp4AtomHeader &atom, std::size_t maxPayloadSize)
{
    if (atom.payloadOffset > atom.atomEnd)
    {
        return {};
    }

    const std::uintmax_t payloadSize = atom.atomEnd - atom.payloadOffset;
    if (payloadSize > maxPayloadSize || payloadSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        return {};
    }

    return ReadRange(input, atom.payloadOffset, static_cast<std::size_t>(payloadSize), maxPayloadSize);
}

constexpr std::array<std::string_view, 192> Id3v1Genres{
    "Blues",
    "Classic Rock",
    "Country",
    "Dance",
    "Disco",
    "Funk",
    "Grunge",
    "Hip-Hop",
    "Jazz",
    "Metal",
    "New Age",
    "Oldies",
    "Other",
    "Pop",
    "R&B",
    "Rap",
    "Reggae",
    "Rock",
    "Techno",
    "Industrial",
    "Alternative",
    "Ska",
    "Death Metal",
    "Pranks",
    "Soundtrack",
    "Euro-Techno",
    "Ambient",
    "Trip-Hop",
    "Vocal",
    "Jazz+Funk",
    "Fusion",
    "Trance",
    "Classical",
    "Instrumental",
    "Acid",
    "House",
    "Game",
    "Sound Clip",
    "Gospel",
    "Noise",
    "AlternRock",
    "Bass",
    "Soul",
    "Punk",
    "Space",
    "Meditative",
    "Instrumental Pop",
    "Instrumental Rock",
    "Ethnic",
    "Gothic",
    "Darkwave",
    "Techno-Industrial",
    "Electronic",
    "Pop-Folk",
    "Eurodance",
    "Dream",
    "Southern Rock",
    "Comedy",
    "Cult",
    "Gangsta",
    "Top 40",
    "Christian Rap",
    "Pop/Funk",
    "Jungle",
    "Native American",
    "Cabaret",
    "New Wave",
    "Psychadelic",
    "Rave",
    "Showtunes",
    "Trailer",
    "Lo-Fi",
    "Tribal",
    "Acid Punk",
    "Acid Jazz",
    "Polka",
    "Retro",
    "Musical",
    "Rock & Roll",
    "Hard Rock",
    "Folk",
    "Folk-Rock",
    "National Folk",
    "Swing",
    "Fast Fusion",
    "Bebob",
    "Latin",
    "Revival",
    "Celtic",
    "Bluegrass",
    "Avantgarde",
    "Gothic Rock",
    "Progressive Rock",
    "Psychedelic Rock",
    "Symphonic Rock",
    "Slow Rock",
    "Big Band",
    "Chorus",
    "Easy Listening",
    "Acoustic",
    "Humour",
    "Speech",
    "Chanson",
    "Opera",
    "Chamber Music",
    "Sonata",
    "Symphony",
    "Booty Bass",
    "Primus",
    "Porn Groove",
    "Satire",
    "Slow Jam",
    "Club",
    "Tango",
    "Samba",
    "Folklore",
    "Ballad",
    "Power Ballad",
    "Rhythmic Soul",
    "Freestyle",
    "Duet",
    "Punk Rock",
    "Drum Solo",
    "A Cappella",
    "Euro-House",
    "Dance Hall",
    "Goa",
    "Drum & Bass",
    "Club-House",
    "Hardcore",
    "Terror",
    "Indie",
    "BritPop",
    "Negerpunk",
    "Polsk Punk",
    "Beat",
    "Christian Gangsta Rap",
    "Heavy Metal",
    "Black Metal",
    "Crossover",
    "Contemporary Christian",
    "Christian Rock",
    "Merengue",
    "Salsa",
    "Thrash Metal",
    "Anime",
    "JPop",
    "Synthpop",
    "Abstract",
    "Art Rock",
    "Baroque",
    "Bhangra",
    "Big Beat",
    "Breakbeat",
    "Chillout",
    "Downtempo",
    "Dub",
    "EBM",
    "Eclectic",
    "Electro",
    "Electroclash",
    "Emo",
    "Experimental",
    "Garage",
    "Global",
    "IDM",
    "Illbient",
    "Industro-Goth",
    "Jam Band",
    "Krautrock",
    "Leftfield",
    "Lounge",
    "Math Rock",
    "New Romantic",
    "Nu-Breakz",
    "Post-Punk",
    "Post-Rock",
    "Psytrance",
    "Shoegaze",
    "Space Rock",
    "Trop Rock",
    "World Music",
    "Neoclassical",
    "Audiobook",
    "Audio Theatre",
    "Neue Deutsche Welle",
    "Podcast",
    "Indie Rock",
    "G-Funk",
    "Dubstep",
    "Garage Rock",
    "Psybient",
};

std::string NormalizeId3Genre(std::string_view value)
{
    auto genreFromIndex = [](std::string_view digits) -> std::string
    {
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch)
                                           { return std::isdigit(ch) != 0; }))
        {
            return {};
        }
        const uint16_t index = ParseUInt16(std::string(digits));
        if (index < Id3v1Genres.size())
        {
            return std::string(Id3v1Genres[index]);
        }
        return {};
    };

    auto normalizeOne = [&](std::string_view raw) -> std::string
    {
        std::string text = TrimText(std::string(raw));
        if (text.empty())
        {
            return {};
        }

        if (text.front() == '(')
        {
            const auto close = text.find(')');
            if (close != std::string::npos)
            {
                const std::string mapped = genreFromIndex(std::string_view(text).substr(1, close - 1));
                const std::string suffix = TrimText(text.substr(close + 1));
                if (!suffix.empty())
                {
                    return suffix;
                }
                if (!mapped.empty())
                {
                    return mapped;
                }
            }
        }

        const std::string mapped = genreFromIndex(text);
        return mapped.empty() ? text : mapped;
    };

    std::size_t start = 0;
    while (start <= value.size())
    {
        const std::size_t end = value.find('\0', start);
        const std::string normalized = normalizeOne(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!normalized.empty())
        {
            return normalized;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }

    return {};
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

    mediaInfo.sampleRate = ClampToUint32(audioStream->codecpar->sample_rate);

    // 比特率优先取音频流自身值，缺失时退回容器级比特率。
    mediaInfo.bitRate = audioStream->codecpar->bit_rate > 0 ? ClampToUint32(audioStream->codecpar->bit_rate) : ClampToUint32(formatContext->bit_rate);

#if LIBAVCODEC_VERSION_MAJOR >= 59
    mediaInfo.channels = ClampToUint8(audioStream->codecpar->ch_layout.nb_channels);
#else
    mediaInfo.channels = ClampToUint8(audioStream->codecpar->channels);
#endif

    mediaInfo.bitDepth = DetectAudioBitDepth(audioStream->codecpar);

    mediaInfo.format = NormalizeContainerFormatName(context);

    return mediaInfo;
}

TagReader::RawMetadata TagReader::ReadMetadata(ReadContext &context)
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
        catch (const std::runtime_error &)
        {
        }
    };

    // 这里只做容器分发，不承载具体字段解析逻辑。
    switch (context.detectedContainer)
    {
    case DetectedContainer::Mp4:
        ignoreMalformedMetadata([&]()
                                { ReadMP4Metadata(context, metadata); });
        break;
    case DetectedContainer::Flac:
    case DetectedContainer::OggVorbis:
        ignoreMalformedMetadata([&]()
                                { ReadVorbisCommentMetadata(context, metadata); });
        break;
    case DetectedContainer::Mp3:
        ignoreMalformedMetadata([&]()
                                { ReadID3v2Metadata(context, metadata); });
        ignoreMalformedMetadata([&]()
                                { ReadID3v1Metadata(context, metadata); });
        break;
    case DetectedContainer::Unknown:
        // 未知容器先尝试最常见的尾部标签，失败则保持空值。
        ignoreMalformedMetadata([&]()
                                { ReadID3v1Metadata(context, metadata); });
        break;
    }

    // 评分和播放次数保持固定值，不参与元数据读取。
    metadata.playCount = 0;
    metadata.rating = 0;

    NormalizeMetadata(metadata);

    return metadata;
}

TagReader::DetectedContainer TagReader::DetectContainer(ReadContext &context)
{
    if (!context.input.is_open())
    {
        return DetectedContainer::Unknown;
    }

    const std::vector<uint8_t> header = ReadRange(context.input, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(context.fileSize, 12)));
    if (header.size() >= 3 && std::memcmp(header.data(), "ID3", 3) == 0)
    {
        return DetectedContainer::Mp3;
    }
    if (header.size() >= 4 && std::string_view(reinterpret_cast<const char *>(header.data()), 4) == "fLaC")
    {
        return DetectedContainer::Flac;
    }
    if (header.size() >= 4 && std::string_view(reinterpret_cast<const char *>(header.data()), 4) == "OggS")
    {
        return DetectedContainer::OggVorbis;
    }
    if (header.size() >= 8 && std::string_view(reinterpret_cast<const char *>(header.data() + 4), 4) == "ftyp")
    {
        return DetectedContainer::Mp4;
    }

    const std::string container = ToLower(context.containerName);
    if (ContainsAny(container, {"mp4", "mov", "m4"}))
    {
        return DetectedContainer::Mp4;
    }
    if (ContainsAny(container, {"ogg", "vorbis"}))
    {
        return DetectedContainer::OggVorbis;
    }
    if (container.find("flac") != std::string::npos)
    {
        return DetectedContainer::Flac;
    }
    if (ContainsAny(container, {"mp3", "mpeg"}))
    {
        return DetectedContainer::Mp3;
    }

    return DetectedContainer::Unknown;
}

std::string TagReader::NormalizeContainerFormatName(const ReadContext &context)
{
    switch (context.detectedContainer)
    {
    case DetectedContainer::Mp3:
        return "mp3";
    case DetectedContainer::Flac:
        return "flac";
    case DetectedContainer::OggVorbis:
        return "ogg";
    case DetectedContainer::Mp4:
    {
        std::string extension = ToLower(context.filePath.extension().string());
        if (!extension.empty() && extension.front() == '.')
        {
            extension.erase(extension.begin());
        }
        return extension == "mp4" ? "mp4" : "m4a";
    }
    case DetectedContainer::Unknown:
        break;
    }

    return NormalizeFormatName(context.filePath, context.containerName);
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

    auto readField = [&](std::size_t offset, std::size_t size)
    {
        // ID3v1 has no encoding marker, so sniff raw bytes before converting to UTF-8.
        const DecodedField field = DecodeRawText(std::string_view(buffer.data() + offset, size));
        return field.success ? field.value : std::string{};
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
    if (metadata.year == 0)
    {
        metadata.year = ParseYearOnly(readField(93, 4));
    }
    if (metadata.genre.empty())
    {
        const auto genreIndex = static_cast<unsigned char>(buffer[127]);
        if (genreIndex < Id3v1Genres.size())
        {
            metadata.genre = std::string(Id3v1Genres[genreIndex]);
        }
    }

    // ID3v1.1 只有在 comment 第 29 字节为 0 且 track byte 非 0 时才表示 track number。
    if (metadata.trackNumber == 0 && buffer[125] == '\0' && buffer[126] != '\0')
    {
        metadata.trackNumber = static_cast<uint16_t>(static_cast<unsigned char>(buffer[126]));
    }
}

void TagReader::ReadID3v2Metadata(ReadContext &context, RawMetadata &metadata)
{
    Id3TagView tagView{};
    if (!ReadId3TagBytes(context, tagView))
    {
        return;
    }

    if (tagView.versionMajor == 2)
    {
        ReadID3v22Frames(context, metadata, tagView.bytes, 0);
        return;
    }

    ReadID3v23Or24Frames(context, metadata, tagView.bytes, tagView.versionMajor, tagView.cursor, tagView.limit);
}

bool TagReader::ReadId3TagBytes(ReadContext &context, Id3TagView &tagView)
{
    tagView = {};
    if (!context.input.is_open())
    {
        return false;
    }

    const std::vector<uint8_t> header = ReadRange(context.input, 0, 10);
    if (header.size() != 10 || std::memcmp(header.data(), "ID3", 3) != 0)
    {
        return false;
    }

    const uint8_t versionMajor = header[3];
    const uint8_t flags = header[5];
    if (versionMajor < 2 || versionMajor > 4)
    {
        return false;
    }
    if (!IsValidSyncSafe32(header.data() + 6))
    {
        return false;
    }

    const uint32_t tagSize = ReadSyncSafe32(header.data() + 6);
    if (tagSize > kMaxId3TagBytes)
    {
        return false;
    }

    std::uintmax_t tagEnd = 0;
    if (!TryAddUintmax(10, tagSize, tagEnd) || tagEnd > context.fileSize)
    {
        return false;
    }

    std::vector<uint8_t> tagBytes = ReadRange(context.input, 10, tagSize, kMaxId3TagBytes);
    if (tagBytes.size() != tagSize)
    {
        return false;
    }
    if ((flags & 0x80) != 0)
    {
        tagBytes = RemoveId3Unsynchronization(std::move(tagBytes));
    }

    std::size_t cursor = 0;
    std::size_t frameLimit = tagBytes.size();
    if (!PrepareId3v24FrameRegion(tagBytes, versionMajor, flags, cursor, frameLimit))
    {
        return false;
    }

    if (versionMajor >= 3 && (flags & 0x40) != 0)
    {
        if (versionMajor == 3)
        {
            if (!PrepareId3v23ExtendedHeader(tagBytes, frameLimit, cursor))
            {
                return false;
            }
        }
        else if (versionMajor == 4)
        {
            // v2.4 extended header / footer region has already been validated above.
        }
        else
        {
            return false;
        }
    }

    tagView.versionMajor = versionMajor;
    tagView.flags = flags;
    tagView.cursor = cursor;
    tagView.limit = frameLimit;
    tagView.bytes = std::move(tagBytes);
    return true;
}

void TagReader::ReadID3v22Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, std::size_t cursor)
{
    while (cursor + 6 <= tagBytes.size())
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        if (frameHeader[0] == 0)
        {
            break;
        }

        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 3);
        if (!IsLikelyId3v22FrameId(frameId))
        {
            break;
        }

        const uint32_t frameSize = ReadBE24(frameHeader + 3);
        if (frameSize == 0)
        {
            break;
        }
        if (cursor + 6 + frameSize > tagBytes.size())
        {
            break;
        }

        const uint8_t *frameData = tagBytes.data() + cursor + 6;
        ReadID3v22Frame(context, metadata, frameId, frameData, frameSize);

        cursor += 6 + static_cast<std::size_t>(frameSize);
    }
}

void TagReader::ReadID3v23Or24Frames(ReadContext &context, RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, std::size_t cursor, std::size_t limit)
{
    limit = std::min(limit, tagBytes.size());
    while (cursor + 10 <= limit)
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        if (frameHeader[0] == 0)
        {
            break;
        }

        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 4);
        if (!IsLikelyId3FrameId(frameId))
        {
            break;
        }

        uint32_t frameSize = 0;
        if (versionMajor >= 4)
        {
            if (!IsValidSyncSafe32(frameHeader + 4))
            {
                break;
            }
            frameSize = ReadSyncSafe32(frameHeader + 4);
        }
        else
        {
            frameSize = ReadBE32(frameHeader + 4);
        }

        if (frameSize == 0)
        {
            break;
        }
        if (frameSize > limit - cursor - 10)
        {
            break;
        }

        const uint16_t frameFlags = static_cast<uint16_t>((frameHeader[8] << 8) | frameHeader[9]);
        std::vector<uint8_t> frameData(tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10),
                                       tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10 + frameSize));
        if (!PrepareId3v23Or24FrameData(versionMajor, frameFlags, frameData))
        {
            cursor += 10 + static_cast<std::size_t>(frameSize);
            continue;
        }

        ReadID3v2Frame(context, metadata, frameId, frameData.data(), frameData.size());

        cursor += 10 + static_cast<std::size_t>(frameSize);
    }
}

void TagReader::ReadID3v22Frame(ReadContext &context, RawMetadata &metadata, std::string_view frameId, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameId == "PIC")
    {
        ReadID3v22PictureFrame(context, metadata, frameData, frameSize);
        return;
    }

    const std::string value = ReadId3TextFrame(frameData, frameSize);
    if (value.empty())
    {
        return;
    }

    if (frameId == "TT2")
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (frameId == "TP1")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (frameId == "TAL")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (frameId == "TP2")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (frameId == "TCM")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (frameId == "TCO")
    {
        if (metadata.genre.empty())
            metadata.genre = NormalizeId3Genre(value);
    }
    else if (frameId == "TYE")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (frameId == "TRK")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (frameId == "TPA")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void TagReader::ReadID3v22PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameData == nullptr || frameSize < 6)
    {
        return;
    }

    const uint8_t encoding = frameData[0];
    const std::string imageFormat(reinterpret_cast<const char *>(frameData + 1), 3);
    const uint8_t pictureType = frameData[4];
    const uint8_t *payload = frameData + 5;
    const std::size_t payloadSize = frameSize - 5;
    if (pictureType != 3)
    {
        return;
    }

    // ID3v2.2 PIC stores description immediately after picture type; image bytes start after its terminator.
    const std::size_t descSize = FindEncodedTerminator(payload, payloadSize, encoding);
    if (descSize >= payloadSize)
    {
        return;
    }
    const std::size_t cursor = descSize + EncodedTerminatorWidth(encoding);
    if (cursor >= payloadSize)
    {
        return;
    }

    (void)imageFormat;
    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, payload + cursor, payloadSize - cursor);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
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
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (frameId == "TPE1")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (frameId == "TALB")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (frameId == "TPE2")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (frameId == "TCOM")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (frameId == "TCON")
    {
        if (metadata.genre.empty())
            metadata.genre = NormalizeId3Genre(value);
    }
    else if (frameId == "TYER" || frameId == "TDRC")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (frameId == "TRCK")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (frameId == "TPOS")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
}

void TagReader::ReadID3v2PictureFrame(ReadContext &context, RawMetadata &metadata, const uint8_t *frameData, std::size_t frameSize)
{
    if (frameData == nullptr || frameSize < 4)
    {
        return;
    }

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

    const uint8_t pictureType = payload[cursor];
    if (pictureType != 3)
    {
        return;
    }
    ++cursor;
    if (cursor >= payloadSize)
    {
        return;
    }
    const std::size_t descSize = FindEncodedTerminator(payload + cursor, payloadSize - cursor, encoding);
    if (descSize >= payloadSize - cursor)
    {
        return;
    }
    cursor += descSize + EncodedTerminatorWidth(encoding);
    if (cursor >= payloadSize)
    {
        return;
    }

    ReadID3v2ApicPayload(context, metadata, mimeType, pictureType, payload + cursor, payloadSize - cursor);
}

void TagReader::ReadID3v2ApicPayload(ReadContext &context, RawMetadata &metadata, std::string_view mimeType, uint8_t pictureType, const uint8_t *imageData, std::size_t imageSize)
{
    if (pictureType != 3 || imageData == nullptr || imageSize == 0)
    {
        return;
    }

    (void)mimeType;
    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, imageData, imageSize);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}
void TagReader::ReadVorbisCommentMetadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    if (context.detectedContainer == DetectedContainer::Flac)
    {
        const std::vector<uint8_t> signature = ReadRange(context.input, 0, 4);
        if (signature.size() != 4 || std::string_view(reinterpret_cast<const char *>(signature.data()), 4) != "fLaC")
        {
            throw std::runtime_error("invalid FLAC signature");
        }

        ReadFlacMetadataBlocks(context, metadata);
    }
    else if (context.detectedContainer == DetectedContainer::OggVorbis)
    {
        ReadOggVorbisComments(context, metadata);
    }
}

void TagReader::ReadFlacMetadataBlocks(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open() || context.fileSize < 8)
    {
        return;
    }

    std::uintmax_t cursor = 4;
    while (true)
    {
        std::uintmax_t blockHeaderEnd = 0;
        if (!TryAddUintmax(cursor, 4, blockHeaderEnd) || blockHeaderEnd > context.fileSize)
        {
            break;
        }

        const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
        if (blockHeader.size() != 4)
        {
            throw std::runtime_error("failed to read FLAC metadata block header");
        }

        const bool lastBlock = (blockHeader[0] & 0x80) != 0;
        const uint32_t blockType = blockHeader[0] & 0x7F;
        const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
        cursor += 4;

        std::uintmax_t blockEnd = 0;
        if (!TryAddUintmax(cursor, blockSize, blockEnd) || blockEnd > context.fileSize)
        {
            throw std::runtime_error("truncated FLAC metadata block");
        }

        if (blockType == 4)
        {
            if (blockSize > kMaxTextFieldBytes)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }

            const std::vector<uint8_t> block = ReadRange(context.input, cursor, static_cast<std::size_t>(blockSize), kMaxTextFieldBytes);
            if (block.size() != blockSize)
            {
                throw std::runtime_error("failed to read FLAC Vorbis comment block");
            }

            const bool ok = ForEachVorbisCommentEntry(block.data(), block.size(), [&](std::string_view entry)
                                                      { ReadVorbisCommentEntry(metadata, entry); });
            if (!ok)
            {
                throw std::runtime_error("invalid FLAC Vorbis comment block");
            }
        }
        else if (blockType == 6)
        {
            if (blockSize > kMaxCoverInputBytes)
            {
                cursor = blockEnd;
                if (lastBlock)
                {
                    break;
                }
                continue;
            }

            const std::vector<uint8_t> picture = ReadRange(context.input, cursor, blockSize, kMaxCoverInputBytes);
            if (picture.size() != blockSize)
            {
                throw std::runtime_error("failed to read FLAC picture block");
            }
            ReadFlacPictureEntry(context, metadata, picture.data(), picture.size());
        }

        cursor = blockEnd;
        if (lastBlock)
        {
            break;
        }
    }
}

void TagReader::ReadOggVorbisComments(ReadContext &context, RawMetadata &metadata)
{
    const bool ok = ReadOggVorbisCommentEntries(context, [&](std::string_view entry)
                                                { ReadVorbisCommentEntry(metadata, entry); });
    (void)ok;
}

bool TagReader::ReadOggVorbisCommentEntries(ReadContext &context, const std::function<void(std::string_view)> &handler)
{
    if (!context.input.is_open())
    {
        return false;
    }

    std::uintmax_t cursor = 0;
    std::vector<uint8_t> packet;
    std::size_t totalScannedBytes = 0;
    std::size_t pageCount = 0;
    std::optional<std::uint32_t> expectedSerial;
    std::uint32_t expectedSequence = 0;
    bool sawIdentificationHeader = false;
    while (true)
    {
        if (++pageCount > kMaxOggPages)
        {
            return false;
        }

        std::uintmax_t pageHeaderEnd = 0;
        if (!TryAddUintmax(cursor, 27, pageHeaderEnd) || pageHeaderEnd > context.fileSize)
        {
            break;
        }

        const std::vector<uint8_t> pageHeader = ReadRange(context.input, cursor, 27);
        if (pageHeader.size() != 27 || std::string_view(reinterpret_cast<const char *>(pageHeader.data()), 4) != "OggS" || pageHeader[4] != 0)
        {
            return false;
        }

        const bool continuation = (pageHeader[5] & 0x01) != 0;
        const std::uint32_t serial = ReadLE32(pageHeader.data() + 14);
        const std::uint32_t sequence = ReadLE32(pageHeader.data() + 18);
        if (!expectedSerial.has_value())
        {
            expectedSerial = serial;
            expectedSequence = sequence;
        }
        else if (serial != *expectedSerial || sequence != expectedSequence + 1)
        {
            return false;
        }
        expectedSequence = sequence;
        if (!packet.empty() && !continuation)
        {
            return false;
        }

        const uint8_t segmentCount = pageHeader[26];
        std::uintmax_t segmentTableOffset = 0;
        if (!TryAddUintmax(cursor, 27, segmentTableOffset))
        {
            return false;
        }

        const std::vector<uint8_t> segmentTable = ReadRange(context.input, segmentTableOffset, segmentCount);
        if (segmentTable.size() != segmentCount)
        {
            return false;
        }

        std::size_t payloadSize = 0;
        for (uint8_t seg : segmentTable)
        {
            payloadSize += seg;
        }
        if (payloadSize > kMaxOggPacketBytes)
        {
            return false;
        }

        std::uintmax_t payloadOffset = 0;
        if (!TryAddUintmax(segmentTableOffset, segmentCount, payloadOffset))
        {
            return false;
        }

        std::uintmax_t nextCursor = 0;
        if (!TryAddUintmax(payloadOffset, payloadSize, nextCursor))
        {
            return false;
        }
        const std::uintmax_t scannedDelta = nextCursor - cursor;
        if (scannedDelta > kMaxOggScannedBytes || totalScannedBytes > kMaxOggScannedBytes - static_cast<std::size_t>(scannedDelta))
        {
            return false;
        }
        totalScannedBytes += static_cast<std::size_t>(scannedDelta);

        const std::vector<uint8_t> payload = ReadRange(context.input, payloadOffset, payloadSize, kMaxOggPacketBytes);
        if (payload.size() != payloadSize)
        {
            return false;
        }

        std::size_t payloadCursor = 0;
        for (uint8_t segmentSize : segmentTable)
        {
            if (payloadCursor + segmentSize > payload.size())
            {
                return false;
            }

            if (segmentSize > kMaxOggPacketBytes - packet.size())
            {
                return false;
            }

            packet.insert(packet.end(), payload.begin() + static_cast<std::ptrdiff_t>(payloadCursor), payload.begin() + static_cast<std::ptrdiff_t>(payloadCursor + segmentSize));
            payloadCursor += segmentSize;

            if (segmentSize < 255)
            {
                if (packet.size() > 7 && packet[0] == 0x01 && std::string_view(reinterpret_cast<const char *>(packet.data() + 1), 6) == "vorbis")
                {
                    sawIdentificationHeader = true;
                }
                else if (sawIdentificationHeader && packet.size() > 7 && packet[0] == 0x03 && std::string_view(reinterpret_cast<const char *>(packet.data() + 1), 6) == "vorbis")
                {
                    const uint8_t *commentData = packet.data() + 7;
                    const std::size_t commentSize = packet.size() - 7;
                    const bool ok = ForEachVorbisCommentEntry(commentData, commentSize, [&](std::string_view entry)
                                                              { handler(entry); });
                    return ok;
                }

                packet.clear();
            }
        }

        cursor = nextCursor;
    }

    return false;
}

void TagReader::ReadVorbisCommentEntry(RawMetadata &metadata, std::string_view entry)
{
    const auto eq = entry.find('=');
    if (eq == std::string_view::npos)
    {
        return;
    }

    const DecodedField keyField = DecodeTextToUtf8(entry.substr(0, eq), "utf-8");
    const DecodedField valueField = DecodeTextToUtf8(entry.substr(eq + 1), "utf-8");
    if (!keyField.success || !valueField.success)
    {
        return;
    }

    const std::string key = ToLower(keyField.value);
    const std::string value = valueField.value;
    if (value.empty())
    {
        return;
    }

    if (key == "title")
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (key == "artist")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (key == "album")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (key == "albumartist")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (key == "album_artist" || key == "album artist")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (key == "composer")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (key == "writer")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (key == "genre")
    {
        if (metadata.genre.empty())
            metadata.genre = value;
    }
    else if (key == "date" || key == "year")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (key == "tracknumber")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (key == "track" || key == "tracknum")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (key == "tracktotal" || key == "totaltracks")
    {
        return;
    }
    else if (key == "discnumber")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
    else if (key == "disc" || key == "discnum")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
    else if (key == "disctotal" || key == "totaldiscs")
    {
        return;
    }
}

void TagReader::ReadFlacPictureEntry(ReadContext &context, RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize)
{
    if (pictureData == nullptr || pictureSize < 32)
    {
        return;
    }

    std::size_t p = 0;
    auto need = [&](std::size_t n)
    { return p + n <= pictureSize; };
    auto skipU32 = [&]()
    {
        if (!need(4))
            return false;
        p += 4;
        return true;
    };

    if (!need(4))
        return;
    const uint32_t pictureType = ReadBE32(pictureData + p);
    p += 4;
    if (!need(4))
        return;
    const uint32_t mimeLen = ReadBE32(pictureData + p);
    p += 4;
    if (!need(mimeLen))
        return;
    const std::string mime = ToLower(std::string(reinterpret_cast<const char *>(pictureData + p), mimeLen));
    p += mimeLen;
    if (!need(4))
        return;
    const uint32_t descLen = ReadBE32(pictureData + p);
    p += 4;
    if (!need(descLen))
        return;
    p += descLen;
    if (!skipU32())
        return;
    if (!skipU32())
        return;
    if (!skipU32())
        return;
    if (!skipU32())
        return;
    if (!need(4))
        return;
    const uint32_t picDataLen = ReadBE32(pictureData + p);
    p += 4;
    if (picDataLen > kMaxCoverInputBytes)
        return;
    if (!need(picDataLen))
        return;

    if (mime == "-->")
    {
        return;
    }

    // 只导出当前歌曲封面；其他 picture type 不作为兜底图片。
    if (pictureType != 3)
    {
        return;
    }

    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, pictureData + p, picDataLen);
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}

void TagReader::ReadMP4Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open())
    {
        return;
    }

    // MP4 入口只负责开始 atom 递归，具体字段映射分散在更小的扫描逻辑里。
    std::size_t visitedAtoms = 0;
    ReadMP4AtomTree(context, metadata, 0, context.fileSize, 0, visitedAtoms);
}

void TagReader::ReadMP4AtomTree(ReadContext &context, RawMetadata &metadata, std::uintmax_t offset, std::uintmax_t limit, std::uint32_t depth, std::size_t &visitedAtoms)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    // depth tracks the strict metadata path: root -> moov -> udta -> meta(full box) -> ilst.
    std::uintmax_t cursor = offset;
    while (cursor < limit)
    {
        if (++visitedAtoms > kMaxMp4Atoms)
        {
            return;
        }

        Mp4AtomHeader atom;
        if (!ReadMp4AtomHeader(context.input, cursor, limit, depth == 0, atom))
        {
            return;
        }

        if (depth == 0 && AtomTypeIs(atom.atomType, "moov"))
        {
            if (atom.payloadOffset < atom.atomEnd)
            {
                ReadMP4AtomTree(context, metadata, atom.payloadOffset, atom.atomEnd, 1, visitedAtoms);
            }
        }
        else if (depth == 1 && AtomTypeIs(atom.atomType, "udta"))
        {
            if (atom.payloadOffset < atom.atomEnd)
            {
                ReadMP4AtomTree(context, metadata, atom.payloadOffset, atom.atomEnd, 2, visitedAtoms);
            }
        }
        else if (depth == 2 && AtomTypeIs(atom.atomType, "meta"))
        {
            std::uintmax_t childOffset = 0;
            if (!TryAddUintmax(atom.payloadOffset, 4, childOffset))
            {
                return;
            }
            if (childOffset < atom.atomEnd)
            {
                ReadMP4AtomTree(context, metadata, childOffset, atom.atomEnd, 3, visitedAtoms);
            }
        }
        else if (depth == 3 && AtomTypeIs(atom.atomType, "ilst"))
        {
            if (atom.payloadOffset < atom.atomEnd)
            {
                ReadMP4AtomTree(context, metadata, atom.payloadOffset, atom.atomEnd, 4, visitedAtoms);
            }
        }
        else if (depth == 4 && (AtomTypeIs(atom.atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) || AtomTypeIs(atom.atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) || AtomTypeIs(atom.atomType, "aART") || AtomTypeIs(atom.atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) || AtomTypeIs(atom.atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) || AtomTypeIs(atom.atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) || AtomTypeIs(atom.atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atom.atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())) || AtomTypeIs(atom.atomType, "trkn") || AtomTypeIs(atom.atomType, "disk") || AtomTypeIs(atom.atomType, "covr")))
        {
            ReadMP4ItemAtom(context, metadata, atom.atomType, atom.payloadOffset, atom.atomEnd, visitedAtoms);
        }

        cursor = atom.atomEnd;
        if (atom.atomSize == 0)
        {
            return;
        }
    }
}

void TagReader::ReadMP4ItemAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit, std::size_t &visitedAtoms)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    Mp4AtomHeader atom;
    if (!ReadMp4AtomHeader(context.input, offset, limit, false, atom))
    {
        return;
    }

    if (atom.atomType == "data")
    {
        if (atom.payloadOffset > atom.atomEnd)
        {
            return;
        }

        const std::uintmax_t payloadSize = atom.atomEnd - atom.payloadOffset;
        const std::size_t maxPayloadSize = Mp4MetadataPayloadLimit(atomType);
        if (payloadSize > maxPayloadSize)
        {
            return;
        }

        const std::vector<uint8_t> data = ReadRange(context.input, atom.payloadOffset, static_cast<std::size_t>(payloadSize), maxPayloadSize);
        if (data.size() < 8)
        {
            return;
        }
        const uint32_t dataType = ReadBE32(data.data() + 0);
        ReadMP4DataAtom(context, metadata, atomType, dataType, data.data() + 8, data.size() - 8);
        return;
    }

    // 兼容 `ilst` 的子项：继续扫描内部 item atom。
    std::uintmax_t cursor = atom.payloadOffset;
    const std::uintmax_t atomLimit = atom.atomEnd;
    while (cursor < atomLimit)
    {
        if (++visitedAtoms > kMaxMp4Atoms)
        {
            return;
        }

        Mp4AtomHeader childAtom;
        if (!ReadMp4AtomHeader(context.input, cursor, atomLimit, false, childAtom))
        {
            return;
        }

        if (childAtom.atomType == "data")
        {
            if (childAtom.payloadOffset > childAtom.atomEnd)
            {
                return;
            }

            const std::uintmax_t payloadSize = childAtom.atomEnd - childAtom.payloadOffset;
            const std::size_t maxPayloadSize = Mp4MetadataPayloadLimit(atomType);
            if (payloadSize > maxPayloadSize)
            {
                return;
            }

            const std::vector<uint8_t> data = ReadRange(context.input, childAtom.payloadOffset, static_cast<std::size_t>(payloadSize), maxPayloadSize);
            if (data.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(data.data());
                ReadMP4DataAtom(context, metadata, atomType, dataType, data.data() + 8, data.size() - 8);
            }
        }

        cursor = childAtom.atomEnd;
        if (childAtom.atomSize == 0)
        {
            return;
        }
    }
}

TagReader::DecodedField TagReader::DecodeMp4TextData(std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize == 0 || payloadSize > kMaxTextFieldBytes)
    {
        return {};
    }

    const std::string_view raw(reinterpret_cast<const char *>(payload), payloadSize);
    if (dataType == 1)
    {
        return DecodeTextToUtf8(raw, "utf-8");
    }
    if (dataType == 0)
    {
        return DecodeTextToUtf8(raw, "utf-8");
    }
    if (dataType == 2)
    {
        if (payloadSize % 2 != 0)
        {
            return {};
        }
        return DecodeTextToUtf8(raw, "utf-16be");
    }
    if (dataType == 3)
    {
        if (payloadSize % 2 != 0)
        {
            return {};
        }
        return DecodeTextToUtf8(raw, "utf-16le");
    }

    return {};
}

std::optional<std::uint16_t> TagReader::ParseMp4TrackDiskNumber(const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize < 6 || ReadBE16(payload) != 0)
    {
        return std::nullopt;
    }

    const std::uint16_t index = static_cast<std::uint16_t>(ReadBE16(payload + 2));
    if (index == 0)
    {
        return std::nullopt;
    }

    (void)ReadBE16(payload + 4);
    return index;
}

void TagReader::ReadMP4DataAtom(ReadContext &context, RawMetadata &metadata, std::string_view atomType, std::uint32_t dataType, const uint8_t *payload, std::size_t payloadSize)
{
    if (payload == nullptr || payloadSize == 0)
    {
        return;
    }

    // MP4 的常见文本和数字字段在 data atom 内，这里只处理项目需要的固定字段。
    if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())) || AtomTypeIs(atomType, "aART") || AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
    {
        const DecodedField field = DecodeMp4TextData(dataType, payload, payloadSize);
        if (!field.success)
        {
            return;
        }

        const std::string value = field.value;
        if (value.empty())
        {
            return;
        }

        if (AtomTypeIs(atomType, std::string_view(kMp4TitleAtom.data(), kMp4TitleAtom.size())))
            metadata.title = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4ArtistAtom.data(), kMp4ArtistAtom.size())))
            metadata.artist = value;
        else if (atomType == "aART")
            metadata.albumArtist = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4AlbumAtom.data(), kMp4AlbumAtom.size())))
            metadata.album = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4ComposerAtom.data(), kMp4ComposerAtom.size())))
            metadata.composer = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4GenreAtom.data(), kMp4GenreAtom.size())))
            metadata.genre = value;
        else if (AtomTypeIs(atomType, std::string_view(kMp4DayAtom.data(), kMp4DayAtom.size())) || AtomTypeIs(atomType, std::string_view(kMp4DateAtom.data(), kMp4DateAtom.size())))
            metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
        return;
    }

    if (atomType == "trkn")
    {
        if (dataType != 0 && dataType != 21)
        {
            return;
        }
        const std::optional<std::uint16_t> trackNumber = ParseMp4TrackDiskNumber(payload, payloadSize);
        if (metadata.trackNumber == 0 && trackNumber.has_value())
        {
            metadata.trackNumber = *trackNumber;
        }
    }
    else if (atomType == "disk")
    {
        if (dataType != 0 && dataType != 21)
        {
            return;
        }
        const std::optional<std::uint16_t> discNumber = ParseMp4TrackDiskNumber(payload, payloadSize);
        if (metadata.discNumber == 0 && discNumber.has_value())
        {
            metadata.discNumber = *discNumber;
        }
    }
    else if (atomType == "covr")
    {
        if (payload == nullptr || payloadSize == 0)
        {
            return;
        }

        const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, payload, payloadSize);
        if (!coverPath.empty())
        {
            metadata.coverPath = coverPath;
        }
    }
}

void TagReader::ReadID3Lyrics(ReadContext &context, RawLyrics &lyrics)
{
    Id3TagView tagView{};
    if (!ReadId3TagBytes(context, tagView))
    {
        return;
    }

    if (tagView.versionMajor == 2)
    {
        if (tagView.flags != 0)
        {
            return;
        }

        ReadID3v22LyricsFrames(context, lyrics, tagView.bytes, tagView.cursor);
        return;
    }

    ReadID3v23Or24LyricsFrames(context, lyrics, tagView.bytes, tagView.versionMajor, tagView.cursor, tagView.limit);
}

void TagReader::ReadID3v22LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, std::size_t cursor)
{
    (void)context;
    RawLyrics ultCandidate{};
    RawLyrics sltCandidate{};

    while (cursor + 6 <= tagBytes.size())
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        if (frameHeader[0] == 0)
        {
            break;
        }

        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 3);
        if (!IsLikelyId3v22FrameId(frameId))
        {
            break;
        }

        const uint32_t frameSize = ReadBE24(frameHeader + 3);
        if (frameSize == 0 || cursor + 6 + frameSize > tagBytes.size())
        {
            break;
        }

        const uint8_t *frameData = tagBytes.data() + cursor + 6;
        if (frameId == "ULT" && frameSize > 4)
        {
            std::size_t p = 0;
            const uint8_t encoding = frameData[p++];
            p += 3;
            const std::size_t descSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
            if (descSize < frameSize - p)
            {
                p += descSize + EncodedTerminatorWidth(encoding);
                if (p < frameSize)
                {
                    AppendPlainLyrics(ultCandidate, ReadId3ByteString(frameData + p, frameSize - p, encoding));
                }
            }
        }
        else if (frameId == "SLT" && frameSize > 6 && sltCandidate.timedLines.empty())
        {
            const uint8_t encoding = frameData[0];
            const uint8_t timestampFormat = frameData[4];
            const uint8_t contentType = frameData[5];
            (void)contentType;
            if (timestampFormat == 2)
            {
                std::vector<std::pair<std::chrono::microseconds, std::string>> timedLines;

                std::size_t p = 1 + 3 + 1 + 1;
                const std::size_t descriptorSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
                if (descriptorSize < frameSize - p)
                {
                    p += descriptorSize + EncodedTerminatorWidth(encoding);
                    while (p < frameSize)
                    {
                        const std::size_t textStart = p;
                        const std::size_t lineSize = FindEncodedTerminator(frameData + p, frameSize - p, encoding);
                        if (lineSize >= frameSize - p)
                        {
                            break;
                        }

                        const std::string line = ReadId3ByteString(frameData + textStart, lineSize, encoding);
                        p += lineSize + EncodedTerminatorWidth(encoding);
                        if (p + 4 > frameSize)
                        {
                            break;
                        }

                        const uint32_t timestampMs = ReadBE32(frameData + p);
                        p += 4;
                        std::string normalizedLine = TrimText(line);
                        if (!normalizedLine.empty())
                        {
                            timedLines.emplace_back(std::chrono::microseconds(static_cast<int64_t>(timestampMs) * 1000), std::move(normalizedLine));
                        }
                    }

                    if (!timedLines.empty())
                    {
                        sltCandidate.timedLines = std::move(timedLines);
                    }
                }
            }
        }

        cursor += 6 + static_cast<std::size_t>(frameSize);
    }

    if (!lyrics.text.empty() || !lyrics.timedLines.empty())
    {
        return;
    }
    if (!sltCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(sltCandidate.timedLines);
    }
    else if (!ultCandidate.text.empty())
    {
        lyrics.text = std::move(ultCandidate.text);
    }
}

void TagReader::ReadID3v23Or24LyricsFrames(ReadContext &context, RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, std::size_t cursor, std::size_t limit)
{
    RawLyrics usltCandidate{};
    RawLyrics syltCandidate{};
    RawLyrics txxxCandidate{};

    limit = std::min(limit, tagBytes.size());
    while (cursor + 10 <= limit)
    {
        const uint8_t *frameHeader = tagBytes.data() + cursor;
        if (frameHeader[0] == 0)
        {
            break;
        }

        const std::string frameId(reinterpret_cast<const char *>(frameHeader), 4);
        if (!IsLikelyId3FrameId(frameId))
        {
            break;
        }

        uint32_t frameSize = 0;
        if (versionMajor >= 4)
        {
            if (!IsValidSyncSafe32(frameHeader + 4))
            {
                break;
            }
            frameSize = ReadSyncSafe32(frameHeader + 4);
        }
        else
        {
            frameSize = ReadBE32(frameHeader + 4);
        }
        if (frameSize == 0 || frameSize > limit - cursor - 10)
        {
            break;
        }

        const uint16_t frameFlags = static_cast<uint16_t>((frameHeader[8] << 8) | frameHeader[9]);
        std::vector<uint8_t> frameData(tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10),
                                       tagBytes.begin() + static_cast<std::ptrdiff_t>(cursor + 10 + frameSize));
        if (!PrepareId3v23Or24FrameData(versionMajor, frameFlags, frameData))
        {
            cursor += 10 + static_cast<std::size_t>(frameSize);
            continue;
        }

        if (frameId == "USLT")
        {
            if (usltCandidate.text.empty() && frameData.size() > 4)
            {
                std::size_t p = 0;
                const uint8_t encoding = frameData[p++];
                p += 3;
                const std::size_t descSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                if (descSize >= frameData.size() - p)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }
                p += descSize + EncodedTerminatorWidth(encoding);
                std::string text;
                if (p < frameData.size())
                {
                    text = ReadId3ByteString(frameData.data() + p, frameData.size() - p, encoding);
                }
                AppendPlainLyrics(usltCandidate, std::move(text));
            }
        }
        else if (frameId == "SYLT")
        {
            if (syltCandidate.timedLines.empty() && frameData.size() > 6)
            {
                const uint8_t encoding = frameData[0];
                const uint8_t timestampFormat = frameData[4];
                const uint8_t contentType = frameData[5];
                (void)contentType;
                if (timestampFormat != 2)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }

                std::vector<std::pair<std::chrono::microseconds, std::string>> timedLines;

                std::size_t p = 1 + 3 + 1 + 1;
                const std::size_t descriptorSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                if (descriptorSize >= frameData.size() - p)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }
                p += descriptorSize + EncodedTerminatorWidth(encoding);
                if (p >= frameData.size())
                {
                    break;
                }

                while (p < frameData.size())
                {
                    const std::size_t textStart = p;
                    const std::size_t lineSize = FindEncodedTerminator(frameData.data() + p, frameData.size() - p, encoding);
                    if (lineSize >= frameData.size() - p)
                    {
                        break;
                    }
                    const std::string line = ReadId3ByteString(frameData.data() + textStart, lineSize, encoding);
                    p += lineSize + EncodedTerminatorWidth(encoding);
                    if (p + 4 > frameData.size())
                    {
                        break;
                    }
                    const uint32_t timestampMs = ReadBE32(frameData.data() + p);
                    p += 4;
                    std::string normalizedLine = TrimText(line);
                    if (!normalizedLine.empty())
                    {
                        timedLines.emplace_back(std::chrono::microseconds(static_cast<int64_t>(timestampMs) * 1000), std::move(normalizedLine));
                    }
                }

                if (!timedLines.empty())
                {
                    syltCandidate.timedLines = std::move(timedLines);
                }
            }
        }
        else if (frameId == "TXXX")
        {
            if (txxxCandidate.text.empty() && frameData.size() > 1)
            {
                const uint8_t encoding = frameData[0];
                const uint8_t *payload = frameData.data() + 1;
                const std::size_t payloadSize = frameData.size() - 1;

                const std::size_t descSize = FindEncodedTerminator(payload, payloadSize, encoding);
                if (descSize >= payloadSize)
                {
                    cursor += 10 + static_cast<std::size_t>(frameSize);
                    continue;
                }

                const std::string description = ReadId3ByteString(payload, descSize, encoding);
                const std::string lowerDescription = ToLower(description);
                if (lowerDescription == "lyrics" || lowerDescription == "unsyncedlyrics" || lowerDescription == "lyric" || lowerDescription == "sylt" || lowerDescription == "syncedlyrics")
                {
                    const std::size_t valueOffset = descSize + EncodedTerminatorWidth(encoding);
                    if (valueOffset < payloadSize)
                    {
                        const std::string value = ReadId3ByteString(payload + valueOffset, payloadSize - valueOffset, encoding);
                        AppendPlainLyrics(txxxCandidate, value);
                    }
                }
            }
        }

        cursor += 10 + static_cast<std::size_t>(frameSize);
    }

    if (!lyrics.text.empty() || !lyrics.timedLines.empty())
    {
        return;
    }
    if (!usltCandidate.text.empty())
    {
        lyrics.text = std::move(usltCandidate.text);
    }
    else if (!syltCandidate.timedLines.empty())
    {
        lyrics.timedLines = std::move(syltCandidate.timedLines);
    }
    else if (!txxxCandidate.text.empty())
    {
        lyrics.text = std::move(txxxCandidate.text);
    }
}

void TagReader::ReadVorbisLyrics(ReadContext &context, RawLyrics &lyrics)
{
    if (!context.input.is_open())
    {
        return;
    }

    if (context.detectedContainer == DetectedContainer::Flac)
    {
        const std::vector<uint8_t> signature = ReadRange(context.input, 0, 4);
        if (signature.size() != 4 || std::string_view(reinterpret_cast<const char *>(signature.data()), 4) != "fLaC")
        {
            return;
        }

        std::uintmax_t cursor = 4;
        while (cursor + 4 <= context.fileSize)
        {
            const std::vector<uint8_t> blockHeader = ReadRange(context.input, cursor, 4);
            if (blockHeader.size() != 4)
            {
                break;
            }

            const bool lastBlock = (blockHeader[0] & 0x80) != 0;
            const uint32_t blockType = blockHeader[0] & 0x7F;
            const uint32_t blockSize = ReadBE24(blockHeader.data() + 1);
            cursor += 4;
            if (blockSize > context.fileSize - cursor)
            {
                break;
            }

            if (blockType == 4)
            {
                if (blockSize > kMaxLyricsBytes)
                {
                    break;
                }

                const std::vector<uint8_t> block = ReadRange(context.input, cursor, static_cast<std::size_t>(blockSize), kMaxLyricsBytes);
                if (block.size() != blockSize)
                {
                    break;
                }

                ForEachVorbisCommentEntry(block.data(), block.size(), [&](std::string_view entry)
                                          {
                    const auto eq = entry.find('=');
                    if (eq != std::string_view::npos)
                    {
                        ReadVorbisLyricsEntry(lyrics, entry.substr(0, eq), entry.substr(eq + 1));
                    } });
            }

            cursor += blockSize;
            if (lastBlock)
            {
                break;
            }
        }
        return;
    }

    if (context.detectedContainer == DetectedContainer::OggVorbis)
    {
        const bool ok = ReadOggVorbisCommentEntries(context, [&](std::string_view entry)
                                                    {
            const auto eq = entry.find('=');
            if (eq != std::string_view::npos)
            {
                ReadVorbisLyricsEntry(lyrics, entry.substr(0, eq), entry.substr(eq + 1));
            } });
        (void)ok;
    }
}

void TagReader::ReadMP4Lyrics(ReadContext &context, RawLyrics &lyrics)
{
    if (!context.input.is_open())
    {
        return;
    }

    ReadMP4LyricsAtomTree(context, lyrics, 0, context.fileSize);
}

void TagReader::ReadMP4LyricsAtomTree(ReadContext &context, RawLyrics &lyrics, std::uintmax_t offset, std::uintmax_t limit)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    std::vector<PendingMp4AtomRange> stack;
    stack.push_back(PendingMp4AtomRange{offset, limit, Mp4PathState::Root});
    std::size_t visitedAtoms = 0;

    while (!stack.empty())
    {
        const PendingMp4AtomRange range = stack.back();
        stack.pop_back();

        std::uintmax_t cursor = range.offset;
        while (cursor < range.limit)
        {
            if (++visitedAtoms > kMaxMp4Atoms)
            {
                return;
            }

            Mp4AtomHeader atom;
            if (!ReadMp4AtomHeader(context.input, cursor, range.limit, range.state == Mp4PathState::Root, atom))
            {
                return;
            }

            if (range.state == Mp4PathState::Ilst && AtomTypeIs(atom.atomType, std::string_view(kMp4LyricsAtom.data(), kMp4LyricsAtom.size())))
            {
                ReadMP4LyricsItem(context, lyrics, atom.atomType, atom.payloadOffset, atom.atomEnd);
            }
            else if (range.state == Mp4PathState::Ilst && atom.atomType == "----")
            {
                if (lyrics.text.empty() && lyrics.timedLines.empty())
                {
                    ReadMP4FreeformLyricsItem(context, lyrics, atom.payloadOffset, atom.atomEnd);
                }
            }
            else
            {
                Mp4PathState childState = range.state;
                std::uintmax_t childOffset = atom.payloadOffset;
                bool descend = false;

                if (range.state == Mp4PathState::Root && atom.atomType == "moov")
                {
                    childState = Mp4PathState::Moov;
                    descend = true;
                }
                else if (range.state == Mp4PathState::Moov && atom.atomType == "udta")
                {
                    childState = Mp4PathState::Udta;
                    descend = true;
                }
                else if (range.state == Mp4PathState::Udta && atom.atomType == "meta")
                {
                    if (!TryAddUintmax(atom.payloadOffset, 4, childOffset))
                    {
                        return;
                    }
                    childState = Mp4PathState::Meta;
                    descend = true;
                }
                else if (range.state == Mp4PathState::Meta && atom.atomType == "ilst")
                {
                    childState = Mp4PathState::Ilst;
                    descend = true;
                }

                if (descend && childOffset < atom.atomEnd)
                {
                    stack.push_back(PendingMp4AtomRange{childOffset, atom.atomEnd, childState});
                }
            }

            cursor = atom.atomEnd;
            if (atom.atomSize == 0)
            {
                break;
            }
        }
    }
}

void TagReader::ReadVorbisLyricsEntry(RawLyrics &lyrics, std::string_view key, std::string_view value)
{
    const DecodedField keyField = DecodeTextToUtf8(key, "utf-8");
    const DecodedField valueField = DecodeTextToUtf8(value, "utf-8");
    if (!keyField.success || !valueField.success)
    {
        return;
    }

    const std::string lowerKey = ToLower(keyField.value);
    if (lowerKey == "lyrics" || lowerKey == "unsyncedlyrics" || lowerKey == "lyric")
    {
        ReadLyricsFromPlainText(lyrics, valueField.value);
    }
    else if (lowerKey == "sylt" || lowerKey == "syncedlyrics")
    {
        ReadLyricsFromPlainText(lyrics, valueField.value);
    }
}

void TagReader::ReadLyricsFromPlainText(RawLyrics &lyrics, std::string_view text)
{
    std::string plain;
    std::vector<std::pair<std::chrono::microseconds, std::string>> timed;
    bool skippedInvalidTimestampLine = false;

    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find_first_of("\r\n", start);
        const std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);

        if (!line.empty())
        {
            std::size_t scan = 0;
            bool sawBracketToken = false;
            bool invalidTimestampToken = false;
            std::vector<std::chrono::microseconds> timestamps;
            while (scan < line.size() && line[scan] == '[')
            {
                const std::size_t close = line.find(']', scan);
                if (close == std::string_view::npos)
                {
                    break;
                }
                sawBracketToken = true;

                std::chrono::microseconds ts{};
                if (!ParseLrcTimestamp(line.substr(scan, close - scan + 1), ts))
                {
                    invalidTimestampToken = true;
                    break;
                }

                timestamps.push_back(ts);
                scan = close + 1;
            }

            if (!timestamps.empty() && !invalidTimestampToken)
            {
                const std::string lyricText = TrimText(std::string(line.substr(scan)));
                if (!lyricText.empty())
                {
                    for (const auto ts : timestamps)
                    {
                        timed.emplace_back(ts, lyricText);
                    }
                }
            }
            else
            {
                if (sawBracketToken)
                {
                    skippedInvalidTimestampLine = true;
                }
                else
                {
                    if (!plain.empty())
                    {
                        plain.push_back('\n');
                    }
                    plain.append(std::string(line));
                }
            }
        }

        if (end == std::string_view::npos)
        {
            break;
        }

        start = end + 1;
        while (start < text.size() && (text[start] == '\r' || text[start] == '\n'))
        {
            ++start;
        }
    }

    if (!timed.empty())
    {
        lyrics.timedLines = std::move(timed);
    }
    else
    {
        if (!plain.empty() || !skippedInvalidTimestampLine)
        {
            AppendPlainLyrics(lyrics, std::move(plain.empty() ? std::string(text) : plain));
        }
    }
}

TagReader::RawLyrics TagReader::ReadLyrics(ReadContext &context)
{
    RawLyrics lyrics{};
    if (!context.input.is_open())
    {
        return lyrics;
    }

    try
    {
        // 歌词入口只负责分发，不直接承载解析细节。
        switch (context.detectedContainer)
        {
        case DetectedContainer::Mp3:
            ReadID3Lyrics(context, lyrics);
            break;
        case DetectedContainer::Flac:
        case DetectedContainer::OggVorbis:
            ReadVorbisLyrics(context, lyrics);
            break;
        case DetectedContainer::Mp4:
            ReadMP4Lyrics(context, lyrics);
            break;
        case DetectedContainer::Unknown:
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

TagReader::DecodedField TagReader::NormalizeText(std::string_view value)
{
    // NormalizeText is the public normalization point for raw tag bytes: sniff first, then convert to UTF-8.
    return DecodeRawText(value);
}

std::string TagReader::DetectTextEncoding(std::string_view raw)
{
    if (raw.empty())
    {
        return "utf-8";
    }

    const auto byteAt = [&](std::size_t index)
    {
        return static_cast<unsigned char>(raw[index]);
    };

    if (raw.size() >= 3 && byteAt(0) == 0xEF && byteAt(1) == 0xBB && byteAt(2) == 0xBF)
    {
        return "utf-8";
    }
    if (raw.size() >= 2 && byteAt(0) == 0xFF && byteAt(1) == 0xFE)
    {
        return "utf-16le";
    }
    if (raw.size() >= 2 && byteAt(0) == 0xFE && byteAt(1) == 0xFF)
    {
        return "utf-16be";
    }

    if (IsValidUtf8(raw))
    {
        return "utf-8";
    }

    if (LooksLikeUtf16WithoutBom(raw, false))
    {
        return "utf-16le";
    }
    if (LooksLikeUtf16WithoutBom(raw, true))
    {
        return "utf-16be";
    }

    return DetectLegacyLocalEncoding(raw);
}

TagReader::DecodedField TagReader::DecodeTextToUtf8(std::string_view raw, std::string_view encoding)
{
    DecodedField field{};
    field.encoding.assign(encoding.begin(), encoding.end());

    const auto fail = [&field]()
    {
        field.value.clear();
        field.success = false;
        return field;
    };

    if (encoding == "utf-8")
    {
        field.value.assign(raw.begin(), raw.end());
        RemoveUtf8Bom(field.value);
        field.value = TrimText(std::move(field.value));
        field.success = IsValidUtf8(field.value);
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    if (encoding == "utf-16le" || encoding == "utf-16be")
    {
        field.value = ReadUtf16Text(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), encoding == "utf-16be");
        field.success = !field.value.empty() || raw.empty();
        if (field.success)
        {
            field.success = IsValidUtf8(field.value);
        }
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    if (encoding == "latin-1")
    {
        field.value = ReadLatin1Text(reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
        field.success = IsValidUtf8(field.value);
        if (!field.success)
        {
            return fail();
        }
        return field;
    }

    field.value = ReadLocaleEncodedText(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), encoding);
    field.success = IsValidUtf8(field.value) && IsMostlyPrintableText(field.value);
    if (!field.success)
    {
        return fail();
    }
    return field;
}

TagReader::DecodedField TagReader::DecodeRawText(std::string_view raw)
{
    const std::string encoding = DetectTextEncoding(raw);
    return DecodeTextToUtf8(raw, encoding);
}

void AssignDecoded(std::string &field, std::string value)
{
    field = std::move(value);
}

bool NormalizeAlreadyUtf8Field(std::string &value)
{
    value = TrimText(std::move(value));
    return IsValidUtf8(value);
}

void TagReader::NormalizeMetadata(RawMetadata &metadata)
{
    auto normalize = [](std::string &text)
    {
        std::string normalized = std::move(text);
        if (NormalizeAlreadyUtf8Field(normalized))
        {
            AssignDecoded(text, std::move(normalized));
        }
        else
        {
            text.clear();
        }
    };

    normalize(metadata.title);
    normalize(metadata.genre);
    normalize(metadata.artist);
    normalize(metadata.album);
    normalize(metadata.albumArtist);
    normalize(metadata.composer);
}

void TagReader::NormalizeLyrics(RawLyrics &lyrics)
{
    if (!lyrics.text.empty())
    {
        if (!NormalizeAlreadyUtf8Field(lyrics.text))
        {
            lyrics.text.clear();
        }
    }

    for (auto &line : lyrics.timedLines)
    {
        if (!NormalizeAlreadyUtf8Field(line.second))
        {
            line.second.clear();
        }
    }

    lyrics.timedLines.erase(std::remove_if(lyrics.timedLines.begin(), lyrics.timedLines.end(), [](const auto &line)
                                           { return line.second.empty(); }),
                            lyrics.timedLines.end());
}

void TagReader::AppendPlainLyrics(RawLyrics &lyrics, std::string text)
{
    text = TrimText(std::move(text));
    if (!text.empty())
    {
        lyrics.text = std::move(text);
    }
}

void TagReader::AppendTimedLyrics(RawLyrics &lyrics, std::chrono::microseconds timestamp, std::string text)
{
    text = TrimText(std::move(text));
    if (text.empty())
    {
        return;
    }

    lyrics.timedLines.emplace_back(timestamp, std::move(text));
}

void TagReader::ReadMP4LyricsItem(ReadContext &context, RawLyrics &lyrics, std::string_view atomType, std::uintmax_t offset, std::uintmax_t limit)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    std::uintmax_t cursor = offset;
    while (cursor < limit)
    {
        Mp4AtomHeader atom;
        if (!ReadMp4AtomHeader(context.input, cursor, limit, false, atom))
        {
            return;
        }

        if (atom.atomType == "data")
        {
            const std::vector<uint8_t> data = ReadMp4AtomPayload(context.input, atom, kMaxLyricsBytes);
            if (data.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(data.data());
                if ((AtomTypeIs(atomType, std::string_view(kMp4LyricsAtom.data(), kMp4LyricsAtom.size())) || atom.atomType == std::string(atomType)) && (dataType == 1 || dataType == 0))
                {
                    const DecodedField field = dataType == 1 ? DecodeTextToUtf8(std::string_view(reinterpret_cast<const char *>(data.data() + 8), data.size() - 8), "utf-8") : DecodeRawText(std::string_view(reinterpret_cast<const char *>(data.data() + 8), data.size() - 8));
                    const std::string text = field.success ? field.value : std::string{};
                    if (!text.empty())
                    {
                        AppendPlainLyrics(lyrics, text);
                    }
                }
            }
        }

        cursor = atom.atomEnd;
        if (atom.atomSize == 0)
        {
            return;
        }
    }
}

void TagReader::ReadMP4FreeformLyricsItem(ReadContext &context, RawLyrics &lyrics, std::uintmax_t offset, std::uintmax_t limit)
{
    if (!context.input.is_open() || limit <= offset)
    {
        return;
    }

    std::string mean;
    std::string name;
    std::string text;

    std::uintmax_t cursor = offset;
    while (cursor < limit)
    {
        Mp4AtomHeader atom;
        if (!ReadMp4AtomHeader(context.input, cursor, limit, false, atom))
        {
            return;
        }

        const std::size_t maxPayloadSize = atom.atomType == "data" ? kMaxLyricsBytes : kMaxTextFieldBytes;
        const std::vector<uint8_t> payload = ReadMp4AtomPayload(context.input, atom, maxPayloadSize);
        if (atom.atomType == "mean" || atom.atomType == "name")
        {
            if (payload.size() >= 4)
            {
                const DecodedField field = DecodeRawText(std::string_view(reinterpret_cast<const char *>(payload.data() + 4), payload.size() - 4));
                if (field.success)
                {
                    if (atom.atomType == "mean")
                    {
                        mean = field.value;
                    }
                    else
                    {
                        name = field.value;
                    }
                }
            }
        }
        else if (atom.atomType == "data")
        {
            if (payload.size() >= 8)
            {
                const uint32_t dataType = ReadBE32(payload.data());
                DecodedField field{};
                if (dataType == 1)
                {
                    field = DecodeTextToUtf8(std::string_view(reinterpret_cast<const char *>(payload.data() + 8), payload.size() - 8), "utf-8");
                }
                else if (dataType == 0)
                {
                    field = DecodeRawText(std::string_view(reinterpret_cast<const char *>(payload.data() + 8), payload.size() - 8));
                }

                if (field.success && !field.value.empty())
                {
                    text = std::move(field.value);
                }
            }
        }

        cursor = atom.atomEnd;
        if (atom.atomSize == 0)
        {
            break;
        }
    }

    if (mean == "com.apple.iTunes" && ToLower(name) == "lyrics" && !text.empty())
    {
        ReadLyricsFromPlainText(lyrics, text);
    }
}

bool TagReader::ParseLrcTimestamp(std::string_view token, std::chrono::microseconds &timestamp)
{
    const auto close = token.find(']');
    if (token.empty() || token.front() != '[' || close == std::string_view::npos)
    {
        return false;
    }

    const std::string timePart = std::string(token.substr(1, close - 1));
    const auto colon = timePart.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }

    uint16_t minutes = 0;
    if (!ParseDecimalU16Strict(timePart.substr(0, colon), minutes))
    {
        return false;
    }

    const std::string secondsPart = timePart.substr(colon + 1);
    const auto dot = secondsPart.find('.');
    uint16_t seconds = 0;
    const std::string_view secondsText = dot == std::string::npos ? std::string_view(secondsPart) : std::string_view(secondsPart).substr(0, dot);
    if (!ParseDecimalU16Strict(secondsText, seconds) || seconds >= 60)
    {
        return false;
    }

    uint16_t millis = 0;
    if (dot != std::string::npos)
    {
        std::string_view frac = std::string_view(secondsPart).substr(dot + 1);
        if (frac.empty() || frac.size() > 3)
        {
            return false;
        }
        uint16_t fraction = 0;
        if (!ParseDecimalU16Strict(frac, fraction))
        {
            return false;
        }
        millis = fraction;
        if (frac.size() == 1)
        {
            millis = static_cast<uint16_t>(fraction * 100);
        }
        else if (frac.size() == 2)
        {
            millis = static_cast<uint16_t>(fraction * 10);
        }
    }

    timestamp = std::chrono::minutes(minutes) + std::chrono::seconds(seconds) + std::chrono::milliseconds(millis);
    return true;
}

MusicTag TagReader::BuildMusicTag(const ReadContext &context, const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics)
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
