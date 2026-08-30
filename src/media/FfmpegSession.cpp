#include "media/FfmpegSession.hpp"
#include "platform/PosixCompat.hpp"
#include "profiling/Profiling.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
// Mach-O 没有"未定义弱符号在链接期解析为 0"的语义，ld64 会直接报 undefined，
// 而 -U 链接选项经 LINK_ONLY 传不到间接消费方。改为运行期 dlsym 查找：
// 测试二进制定义了该符号就能找到，生产二进制找不到即为空，无链接期依赖。
#include <dlfcn.h>
#elif defined(__GNUC__) || defined(__clang__)
extern "C" void TagReaderOpenContextAfterInitialOpenHookForTests() __attribute__((weak));
#endif

namespace
{
[[nodiscard]] std::string Utf8Text(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}
}

namespace
{
struct FfmpegAvioState
{
    int fd{-1};
    std::uintmax_t fileSize{};
    std::uintmax_t offset{};
};

void FreeAvioContext(AVIOContext *avioContext) noexcept
{
    if (avioContext == nullptr)
    {
        return;
    }

    delete static_cast<FfmpegAvioState *>(avioContext->opaque);
    avioContext->opaque = nullptr;
    av_freep(&avioContext->buffer);
    avio_context_free(&avioContext);
}
}

void tagreader_core::ReadContext::FormatContextDeleter::operator()(AVFormatContext *context) const noexcept
{
    if (context != nullptr)
    {
        AVIOContext *avioContext = context->pb;
        avformat_close_input(&context);
        FreeAvioContext(avioContext);
    }
}

namespace tagreader_media
{
namespace
{
void RunOpenContextAfterInitialOpenHookForTests()
{
#if defined(__APPLE__)
    using HookFn = void (*)();
    static HookFn const hook = reinterpret_cast<HookFn>(
        ::dlsym(RTLD_DEFAULT, "TagReaderOpenContextAfterInitialOpenHookForTests"));
    if (hook != nullptr)
    {
        hook();
    }
#elif defined(__GNUC__) || defined(__clang__)
    if (TagReaderOpenContextAfterInitialOpenHookForTests != nullptr)
    {
        TagReaderOpenContextAfterInitialOpenHookForTests();
    }
#endif
}

std::string MakeFFmpegError(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
}

int ReadPacketFromFd(void *opaque, std::uint8_t *buffer, int bufferSize)
{
    auto *state = static_cast<FfmpegAvioState *>(opaque);
    if (state == nullptr || state->fd < 0 || buffer == nullptr || bufferSize <= 0)
    {
        return AVERROR(EINVAL);
    }
    if (state->offset >= state->fileSize)
    {
        return AVERROR_EOF;
    }

    const std::uintmax_t remaining = state->fileSize - state->offset;
    const auto bytesToRead = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(bufferSize)));
    if (state->offset > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return AVERROR(EOVERFLOW);
    }

    while (true)
    {
        const ssize_t bytesRead = ::pread(state->fd, buffer, bytesToRead, static_cast<std::int64_t>(state->offset));
        if (bytesRead < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return AVERROR(errno);
        }
        if (bytesRead == 0)
        {
            return AVERROR_EOF;
        }

        state->offset += static_cast<std::uintmax_t>(bytesRead);
        return static_cast<int>(bytesRead);
    }
}

int64_t SeekFd(void *opaque, int64_t offset, int whence)
{
    auto *state = static_cast<FfmpegAvioState *>(opaque);
    if (state == nullptr)
    {
        return AVERROR(EINVAL);
    }
    if (whence == AVSEEK_SIZE)
    {
        return static_cast<int64_t>(state->fileSize);
    }

    whence &= ~AVSEEK_FORCE;
    std::uintmax_t base = 0;
    switch (whence)
    {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = state->offset;
        break;
    case SEEK_END:
        base = state->fileSize;
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (offset < 0)
    {
        const auto backwards = static_cast<std::uintmax_t>(-(offset + 1)) + 1;
        if (backwards > base)
        {
            return AVERROR(EINVAL);
        }
        state->offset = base - backwards;
        return static_cast<int64_t>(state->offset);
    }

    const auto forwards = static_cast<std::uintmax_t>(offset);
    if (base > std::numeric_limits<std::uintmax_t>::max() - forwards)
    {
        return AVERROR(EOVERFLOW);
    }
    state->offset = base + forwards;
    return static_cast<int64_t>(state->offset);
}

AVIOContext *CreateAvioContext(int fd, std::uintmax_t fileSize)
{
    constexpr int kAvioBufferSize = 32768;
    auto *buffer = static_cast<std::uint8_t *>(av_malloc(kAvioBufferSize));
    if (buffer == nullptr)
    {
        throw std::bad_alloc();
    }
    std::unique_ptr<std::uint8_t, decltype(&av_free)> bufferGuard(buffer, av_free);

    auto *state = new FfmpegAvioState{fd, fileSize, 0};
    AVIOContext *avioContext = avio_alloc_context(buffer, kAvioBufferSize, 0, state, ReadPacketFromFd, nullptr, SeekFd);
    if (avioContext == nullptr)
    {
        delete state;
        throw std::runtime_error("failed to allocate FFmpeg AVIO context");
    }
    bufferGuard.release();
    return avioContext;
}

std::filesystem::file_time_type FileTimeFromStat(const tagreader_stat_t &statBuffer)
{
#if defined(_WIN32)
    const auto seconds = std::chrono::seconds(statBuffer.st_mtime);
    const auto nanos = std::chrono::nanoseconds(0);
#elif defined(__APPLE__)
    const auto seconds = std::chrono::seconds(statBuffer.st_mtimespec.tv_sec);
    const auto nanos = std::chrono::nanoseconds(statBuffer.st_mtimespec.tv_nsec);
#else
    const auto seconds = std::chrono::seconds(statBuffer.st_mtim.tv_sec);
    const auto nanos = std::chrono::nanoseconds(statBuffer.st_mtim.tv_nsec);
#endif

    // MSVC 的 system_clock::period 为 100ns（FILETIME 精度），需要 duration_cast 显式转换；
    // Linux/macOS 上 period 为 1ns，此转换是 no-op。
    const std::chrono::system_clock::time_point systemTime(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(seconds + nanos));
    const auto fileClockTime = systemTime - std::chrono::system_clock::now() + std::filesystem::file_time_type::clock::now();
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(fileClockTime);
}
}

void RegisterAllFormatsIfNeeded()
{
#if LIBAVFORMAT_VERSION_MAJOR < 59
    av_register_all();
#endif
}

tagreader_core::ReadContext OpenContext(const std::filesystem::path &filePath)
{
    TAGREADER_PROFILE_SCOPE_COLOR("OpenContext", TAGREADER_COLOR_FFMPEG);
    
    tagreader_core::ReadContext context{};
    context.filePath = filePath;

    std::error_code ec;
    const bool symbolicLink = std::filesystem::is_symlink(filePath, ec);
    if (ec)
    {
        throw std::runtime_error("failed to query symbolic link status: " + ec.message());
    }
    if (symbolicLink)
    {
        throw std::runtime_error("Rejecting symbolic link path: " + Utf8Text(filePath));
    }

#if defined(__unix__) || defined(__APPLE__)
    int openFlags = O_RDONLY;
#if defined(O_CLOEXEC)
    openFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    openFlags |= O_NOFOLLOW;
#endif
    const int fd = ::open(filePath.c_str(), openFlags);
    if (fd < 0)
    {
        throw std::runtime_error("failed to open file input descriptor: " + std::string(std::strerror(errno)));
    }
    context.input.reset(fd);

    struct stat statBuffer
    {
    };
    if (::fstat(context.input.get(), &statBuffer) != 0)
    {
        throw std::runtime_error("failed to stat opened file input descriptor: " + std::string(std::strerror(errno)));
    }
    if (!S_ISREG(statBuffer.st_mode))
    {
        throw std::runtime_error("opened input is not a regular file: " + Utf8Text(filePath));
    }
    if (statBuffer.st_size < 0)
    {
        throw std::runtime_error("opened input has negative file size: " + Utf8Text(filePath));
    }
    context.fileSize = static_cast<std::uintmax_t>(statBuffer.st_size);
    context.lastModified = FileTimeFromStat(statBuffer);
#elif defined(_WIN32)
    const int fd = ::open(filePath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        throw std::runtime_error("failed to open file input descriptor: " + std::string(std::strerror(errno)));
    }
    context.input.reset(fd);

    tagreader_stat_t statBuffer
    {
    };
    if (::fstat(context.input.get(), &statBuffer) != 0)
    {
        throw std::runtime_error("failed to stat opened file input descriptor: " + std::string(std::strerror(errno)));
    }
    if (!S_ISREG(statBuffer.st_mode))
    {
        throw std::runtime_error("opened input is not a regular file: " + Utf8Text(filePath));
    }
    if (statBuffer.st_size < 0)
    {
        throw std::runtime_error("opened input has negative file size: " + Utf8Text(filePath));
    }
    context.fileSize = static_cast<std::uintmax_t>(statBuffer.st_size);
    context.lastModified = FileTimeFromStat(statBuffer);
#else
    throw std::runtime_error("file descriptor input binding is not supported on this platform");
#endif

    RunOpenContextAfterInitialOpenHookForTests();

    AVFormatContext *formatContext = avformat_alloc_context();
    if (formatContext == nullptr)
    {
        throw std::bad_alloc();
    }
    struct FormatContextGuard
    {
        AVFormatContext *context{};

        explicit FormatContextGuard(AVFormatContext *initialContext) noexcept
            : context(initialContext)
        {
        }

        ~FormatContextGuard()
        {
            if (context != nullptr)
            {
                avformat_free_context(context);
            }
        }

        FormatContextGuard(const FormatContextGuard &) = delete;
        FormatContextGuard &operator=(const FormatContextGuard &) = delete;

        void Reset(AVFormatContext *newContext) noexcept
        {
            context = newContext;
        }

        void Release() noexcept
        {
            context = nullptr;
        }
    };
    FormatContextGuard formatContextGuard{formatContext};

    AVIOContext *avioContext = CreateAvioContext(context.input.get(), context.fileSize);
    formatContext->pb = avioContext;

    int openResult;
    {
        TAGREADER_PROFILE_SCOPE_COLOR("avformat_open_input", TAGREADER_COLOR_FFMPEG);
        openResult = avformat_open_input(&formatContext, nullptr, nullptr, nullptr);
    }
    formatContextGuard.Reset(formatContext);
    if (openResult < 0)
    {
        formatContextGuard.Release();
        FreeAvioContext(avioContext);
        if (formatContext != nullptr)
        {
            formatContext->pb = nullptr;
            avformat_free_context(formatContext);
        }
        throw std::runtime_error("avformat_open_input failed: " + MakeFFmpegError(openResult));
    }

    int infoResult;
    {
        TAGREADER_PROFILE_SCOPE_COLOR("avformat_find_stream_info", TAGREADER_COLOR_FFMPEG);
        infoResult = avformat_find_stream_info(formatContext, nullptr);
    }
    if (infoResult < 0)
    {
        formatContextGuard.Release();
        AVIOContext *failedAvioContext = formatContext->pb;
        avformat_close_input(&formatContext);
        FreeAvioContext(failedAvioContext);
        throw std::runtime_error("avformat_find_stream_info failed: " + MakeFFmpegError(infoResult));
    }

    context.formatContext.reset(formatContext);
    formatContextGuard.Release();
    return context;
}
}
