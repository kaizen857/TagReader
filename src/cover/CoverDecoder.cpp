#include "cover/CoverDecoder.hpp"

#include "TagReaderInternal.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef __cplusplus
}
#endif

#include <array>
#include <limits>
#include <memory>

namespace tagreader_cover
{
namespace
{
constexpr tagreader_internal::CoverDecodeLimits kCoverDecodeLimits{};
constexpr std::size_t kMaxUnknownMagicFallbackCodecs = 2;

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
    if (data == nullptr || size == 0 || size > kCoverDecodeLimits.maxInputBytes || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

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

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr)
    {
        return {};
    }
    // data is an external read-only buffer owned by the caller.
    // The AVPacket does NOT own this buffer (packet->buf remains nullptr),
    // so av_packet_free() will NOT attempt to deallocate external memory.
    packet->data = const_cast<uint8_t *>(data);
    packet->size = static_cast<int>(size);

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

    const int scaledRows = sws_scale(swsContext.get(), decodedFrame->data, decodedFrame->linesize, 0, decodedFrame->height, rgbFrame->data, rgbFrame->linesize);
    if (scaledRows != decodedFrame->height)
    {
        return {};
    }

    std::vector<uint8_t> png = EncodeFrameAsPng(rgbFrame.get());
    if (png.size() > kCoverDecodeLimits.maxOutputBytes)
    {
        return {};
    }

    return png;
}
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
            AV_CODEC_ID_MJPEG,
            AV_CODEC_ID_WEBP,
            AV_CODEC_ID_GIF,
            AV_CODEC_ID_TIFF,
            AV_CODEC_ID_BMP,
        };

        for (std::size_t index = 0; index < fallbackCodecs.size() && index < kMaxUnknownMagicFallbackCodecs; ++index)
        {
            png = ConvertImageToPng(data, size, fallbackCodecs[index]);
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

}
