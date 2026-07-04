#ifndef TAGREADER_COVER_COVERDECODER_HPP
#define TAGREADER_COVER_COVERDECODER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFrame;

namespace tagreader_cover
{
std::vector<uint8_t> DecodeAndEncodeCoverPng(const uint8_t *data, std::size_t size);

struct DecodedImage
{
    AVFrame *frame{nullptr};
    int width{0};
    int height{0};
};

struct ThumbnailOptions
{
    uint32_t maxWidth{256};
    uint32_t maxHeight{256};
    bool maintainAspectRatio{true};
    int scalingQuality{2};
};

struct PngEncodeOptions
{
    int compressionLevel{6};
};

DecodedImage DecodeImage(const uint8_t *data, std::size_t size);
DecodedImage DecodeImageToRgb24Direct(const uint8_t *data, std::size_t size);
void FreeDecodedImage(DecodedImage &image);
DecodedImage GenerateThumbnail(const DecodedImage &original, const ThumbnailOptions &options);
std::vector<uint8_t> EncodePngWithOptions(const DecodedImage &image, const PngEncodeOptions &options);
}

#endif
