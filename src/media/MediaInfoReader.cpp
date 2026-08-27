#include "media/MediaInfoReader.hpp"
#include "profiling/Profiling.hpp"

#include "common/ParseHelpers.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace tagreader_media
{
namespace
{
using tagreader_common::ToLower;
using tagreader_core::DetectedContainer;

[[nodiscard]] std::string Utf8Text(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
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

    std::string extension = ToLower(Utf8Text(filePath.extension()));
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

std::string NormalizeContainerFormatName(const tagreader_core::ReadContext &context)
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
        std::string extension = ToLower(Utf8Text(context.filePath.extension()));
        if (!extension.empty() && extension.front() == '.')
        {
            extension.erase(extension.begin());
        }
        return extension == "mp4" ? "mp4" : "m4a";
    }
    case DetectedContainer::Ape:
    case DetectedContainer::Unknown:
        break;
    }

    return NormalizeFormatName(context.filePath, context.containerName);
}
}

void DetectStream(tagreader_core::ReadContext &context)
{
    if (context.formatContext == nullptr)
    {
        throw std::runtime_error("format context is not initialized");
    }

    const AVFormatContext *formatContext = context.formatContext.get();
    context.audioStreamIndex = -1;
    context.containerName.clear();

    if (formatContext->iformat != nullptr)
    {
        if (formatContext->iformat->name != nullptr)
        {
            context.containerName = formatContext->iformat->name;
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

    const AVStream *audioStream = formatContext->streams[context.audioStreamIndex];
    if (audioStream == nullptr || audioStream->codecpar == nullptr)
    {
        throw std::runtime_error("audio stream information is incomplete");
    }
}

tagreader_core::RawMediaInfo ReadMediaInfo(const tagreader_core::ReadContext &context)
{
    TAGREADER_PROFILE_SCOPE_COLOR("ReadMediaInfo", TAGREADER_COLOR_FFMPEG);
    
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

    tagreader_core::RawMediaInfo mediaInfo{};

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
}
