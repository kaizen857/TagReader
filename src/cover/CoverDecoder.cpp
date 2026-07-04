#include "cover/CoverDecoder.hpp"

#include "profiling/Profiling.hpp"
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
#include <cstring>
#include <span>
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
    TAGREADER_PROFILE_FUNCTION();

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
    {
        TAGREADER_PROFILE_SCOPE_COLOR("png avcodec_open2", TAGREADER_COLOR_FFMPEG);
        if (avcodec_open2(encoderContext.get(), encoder, nullptr) < 0)
        {
            return {};
        }
    }

    {
        TAGREADER_PROFILE_SCOPE_COLOR("png avcodec_send_frame", TAGREADER_COLOR_FFMPEG);
        if (avcodec_send_frame(encoderContext.get(), frame) < 0)
        {
            return {};
        }
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (packet == nullptr)
    {
        return {};
    }

    {
        TAGREADER_PROFILE_SCOPE_COLOR("png avcodec_receive_packet", TAGREADER_COLOR_FFMPEG);
        if (avcodec_receive_packet(encoderContext.get(), packet.get()) < 0)
        {
            return {};
        }
    }

    return std::vector<uint8_t>(packet->data, packet->data + packet->size);
}

bool CopyImageBytesToPacket(AVPacket *packet, std::span<const uint8_t> bytes)
{
    TAGREADER_PROFILE_FUNCTION();

    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    if (av_new_packet(packet, static_cast<int>(bytes.size())) < 0)
    {
        return false;
    }

    std::memcpy(packet->data, bytes.data(), bytes.size());
    return true;
}

bool DecodePacketToFrame(AVCodecContext *decoderContext, AVPacket *packet, AVFrame *decodedFrame)
{
    TAGREADER_PROFILE_FUNCTION();

    {
        TAGREADER_PROFILE_SCOPE_COLOR("image avcodec_send_packet", TAGREADER_COLOR_FFMPEG);
        if (avcodec_send_packet(decoderContext, packet) < 0)
        {
            return false;
        }
    }

    {
        TAGREADER_PROFILE_SCOPE_COLOR("image avcodec_receive_frame", TAGREADER_COLOR_FFMPEG);
        if (avcodec_receive_frame(decoderContext, decodedFrame) < 0)
        {
            return false;
        }
    }

    return true;
}

bool ConvertFrameToRgb24(const AVFrame *decodedFrame, AVFrame *rgbFrame)
{
    TAGREADER_PROFILE_FUNCTION();

    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = decodedFrame->width;
    rgbFrame->height = decodedFrame->height;
    {
        TAGREADER_PROFILE_SCOPE_COLOR("rgb av_frame_get_buffer", TAGREADER_COLOR_DECODE);
        if (av_frame_get_buffer(rgbFrame, 1) < 0)
        {
            return false;
        }
    }

    std::unique_ptr<SwsContext, SwsContextDeleter> swsContext;
    {
        TAGREADER_PROFILE_SCOPE_COLOR("rgb sws_getContext", TAGREADER_COLOR_DECODE);
        swsContext.reset(sws_getContext(decodedFrame->width,
                                        decodedFrame->height,
                                        static_cast<AVPixelFormat>(decodedFrame->format),
                                        rgbFrame->width,
                                        rgbFrame->height,
                                        AV_PIX_FMT_RGB24,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr));
    }
    if (swsContext == nullptr)
    {
        return false;
    }

    int scaledRows = 0;
    {
        TAGREADER_PROFILE_SCOPE_COLOR("rgb sws_scale", TAGREADER_COLOR_DECODE);
        scaledRows = sws_scale(swsContext.get(), decodedFrame->data, decodedFrame->linesize, 0, decodedFrame->height, rgbFrame->data, rgbFrame->linesize);
    }
    return scaledRows == decodedFrame->height;
}

std::vector<uint8_t> ConvertImageToPng(const uint8_t *data, std::size_t size, AVCodecID codecId)
{
    TAGREADER_PROFILE_FUNCTION();

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
    if (decoderContext == nullptr)
    {
        return {};
    }

    {
        TAGREADER_PROFILE_SCOPE_COLOR("image avcodec_open2", TAGREADER_COLOR_FFMPEG);
        if (avcodec_open2(decoderContext.get(), decoder, nullptr) < 0)
        {
            return {};
        }
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
    if (!CopyImageBytesToPacket(packet.get(), std::span<const uint8_t>(data, size)))
    {
        return {};
    }
    if (!DecodePacketToFrame(decoderContext.get(), packet.get(), decodedFrame.get()))
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
    if (!ConvertFrameToRgb24(decodedFrame.get(), rgbFrame.get()))
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
    TAGREADER_PROFILE_FUNCTION();

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


DecodedImage DecodeImageToRgb24Direct(const uint8_t *data, std::size_t size)
{
    TAGREADER_PROFILE_FUNCTION();
    
    if (data == nullptr || size == 0 || size > kCoverDecodeLimits.maxInputBytes || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }
    
    const ImageFormat format = DetectImageFormat(data, size);
    AVCodecID codecId = AV_CODEC_ID_NONE;
    
    if (format == ImageFormat::Png)
    {
        codecId = AV_CODEC_ID_PNG;
    }
    else if (format == ImageFormat::Jpeg)
    {
        codecId = AV_CODEC_ID_MJPEG;
    }
    else if (format == ImageFormat::Bmp)
    {
        codecId = AV_CODEC_ID_BMP;
    }
    else if (format == ImageFormat::Webp)
    {
        codecId = AV_CODEC_ID_WEBP;
    }
    else if (format == ImageFormat::Gif)
    {
        codecId = AV_CODEC_ID_GIF;
    }
    else if (format == ImageFormat::Tiff)
    {
        codecId = AV_CODEC_ID_TIFF;
    }
    else
    {
        return {};
    }
    
    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (decoder == nullptr)
    {
        return {};
    }
    
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> decoderContext(avcodec_alloc_context3(decoder));
    if (decoderContext == nullptr)
    {
        return {};
    }
    
    {
        TAGREADER_PROFILE_SCOPE_COLOR("image avcodec_open2", TAGREADER_COLOR_FFMPEG);
        if (avcodec_open2(decoderContext.get(), decoder, nullptr) < 0)
        {
            return {};
        }
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
    if (!CopyImageBytesToPacket(packet.get(), std::span<const uint8_t>(data, size)))
    {
        return {};
    }
    if (!DecodePacketToFrame(decoderContext.get(), packet.get(), decodedFrame.get()))
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
    if (!ConvertFrameToRgb24(decodedFrame.get(), rgbFrame.get()))
    {
        return {};
    }
    
    DecodedImage result;
    result.frame = rgbFrame.release();
    result.width = result.frame->width;
    result.height = result.frame->height;
    return result;
}

DecodedImage DecodeImage(const uint8_t *data, std::size_t size)
{
    TAGREADER_PROFILE_FUNCTION();
    
    if (data == nullptr || size == 0 || size > kCoverDecodeLimits.maxInputBytes)
    {
        return {};
    }
    
    std::vector<uint8_t> png = DecodeAndEncodeCoverPng(data, size);
    if (png.empty())
    {
        return {};
    }
    
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (decoder == nullptr)
    {
        return {};
    }
    
    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> decoderContext(avcodec_alloc_context3(decoder));
    if (decoderContext == nullptr)
    {
        return {};
    }
    
    if (avcodec_open2(decoderContext.get(), decoder, nullptr) < 0)
    {
        return {};
    }
    
    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    if (frame == nullptr)
    {
        return {};
    }
    
    std::unique_ptr<AVPacket, AvPacketDeleter> pkt(av_packet_alloc());
    if (pkt == nullptr)
    {
        return {};
    }
    
    if (av_new_packet(pkt.get(), static_cast<int>(png.size())) < 0)
    {
        return {};
    }
    
    std::memcpy(pkt->data, png.data(), png.size());
    
    if (avcodec_send_packet(decoderContext.get(), pkt.get()) < 0)
    {
        return {};
    }
    
    if (avcodec_receive_frame(decoderContext.get(), frame.get()) < 0)
    {
        return {};
    }
    
    DecodedImage result;
    result.frame = frame.release();
    result.width = result.frame->width;
    result.height = result.frame->height;
    return result;
}

void FreeDecodedImage(DecodedImage &image)
{
    if (image.frame != nullptr)
    {
        av_frame_free(&image.frame);
        image.frame = nullptr;
        image.width = 0;
        image.height = 0;
    }
}

DecodedImage GenerateThumbnail(const DecodedImage &original, const ThumbnailOptions &options)
{
    TAGREADER_PROFILE_SCOPE("GenerateThumbnail");
    
    if (original.frame == nullptr || original.width <= 0 || original.height <= 0)
    {
        return {};
    }
    
    int targetWidth = static_cast<int>(options.maxWidth);
    int targetHeight = static_cast<int>(options.maxHeight);
    
    if (options.maintainAspectRatio)
    {
        const double aspectRatio = static_cast<double>(original.width) / static_cast<double>(original.height);
        if (aspectRatio > 1.0)
        {
            targetHeight = static_cast<int>(static_cast<double>(targetWidth) / aspectRatio);
        }
        else
        {
            targetWidth = static_cast<int>(static_cast<double>(targetHeight) * aspectRatio);
        }
        
        if (targetWidth <= 0) targetWidth = 1;
        if (targetHeight <= 0) targetHeight = 1;
    }
    
    if (original.width <= targetWidth && original.height <= targetHeight)
    {
        std::unique_ptr<AVFrame, AvFrameDeleter> clonedFrame(av_frame_clone(original.frame));
        if (clonedFrame == nullptr)
        {
            return {};
        }
        
        DecodedImage result;
        result.frame = clonedFrame.release();
        result.width = result.frame->width;
        result.height = result.frame->height;
        return result;
    }
    
    std::unique_ptr<AVFrame, AvFrameDeleter> scaledFrame(av_frame_alloc());
    if (scaledFrame == nullptr)
    {
        return {};
    }
    
    scaledFrame->format = AV_PIX_FMT_RGB24;
    scaledFrame->width = targetWidth;
    scaledFrame->height = targetHeight;
    
    if (av_frame_get_buffer(scaledFrame.get(), 1) < 0)
    {
        return {};
    }
    
    int swsFlags = SWS_FAST_BILINEAR;
    if (options.scalingQuality == 1)
    {
        swsFlags = SWS_BILINEAR;
    }
    else if (options.scalingQuality == 2)
    {
        swsFlags = SWS_LANCZOS;
    }
    
    std::unique_ptr<SwsContext, SwsContextDeleter> swsContext;
    swsContext.reset(sws_getContext(original.width,
                                    original.height,
                                    AV_PIX_FMT_RGB24,
                                    targetWidth,
                                    targetHeight,
                                    AV_PIX_FMT_RGB24,
                                    swsFlags,
                                    nullptr,
                                    nullptr,
                                    nullptr));
    if (swsContext == nullptr)
    {
        return {};
    }
    
    int scaledRows = sws_scale(swsContext.get(), original.frame->data, original.frame->linesize, 0, original.height, scaledFrame->data, scaledFrame->linesize);
    
    if (scaledRows != targetHeight)
    {
        return {};
    }
    
    DecodedImage result;
    result.frame = scaledFrame.release();
    result.width = result.frame->width;
    result.height = result.frame->height;
    return result;
}

std::vector<uint8_t> EncodePngWithOptions(const DecodedImage &image, const PngEncodeOptions &options)
{
    TAGREADER_PROFILE_FUNCTION();
    
    if (image.frame == nullptr || image.width <= 0 || image.height <= 0)
    {
        return {};
    }
    
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
    
    encoderContext->width = image.frame->width;
    encoderContext->height = image.frame->height;
    encoderContext->pix_fmt = AV_PIX_FMT_RGB24;
    encoderContext->time_base = AVRational{1, 1};
    encoderContext->compression_level = options.compressionLevel;
    
    if (avcodec_open2(encoderContext.get(), encoder, nullptr) < 0)
    {
        return {};
    }
    
    if (avcodec_send_frame(encoderContext.get(), image.frame) < 0)
    {
        return {};
    }
    
    std::unique_ptr<AVPacket, AvPacketDeleter> outputPacket(av_packet_alloc());
    if (outputPacket == nullptr)
    {
        return {};
    }
    
    if (avcodec_receive_packet(encoderContext.get(), outputPacket.get()) < 0)
    {
        return {};
    }
    
    return std::vector<uint8_t>(outputPacket->data, outputPacket->data + outputPacket->size);
}
}
