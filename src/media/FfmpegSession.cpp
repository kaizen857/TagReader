#include "media/FfmpegSession.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#ifdef __cplusplus
}
#endif

#include <stdexcept>
#include <system_error>

void tagreader_core::ReadContext::FormatContextDeleter::operator()(AVFormatContext *context) const noexcept
{
    if (context != nullptr)
    {
        avformat_close_input(&context);
    }
}

namespace tagreader_media
{
namespace
{
std::string MakeFFmpegError(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
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
        throw std::runtime_error("Rejecting symbolic link path: " + filePath.string());
    }

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
}
