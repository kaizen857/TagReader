#ifndef TAGREADER_COVER_COVERDECODER_HPP
#define TAGREADER_COVER_COVERDECODER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tagreader_cover
{
std::vector<uint8_t> DecodeAndEncodeCoverPng(const uint8_t *data, std::size_t size);
std::size_t CoverPngMaxOutputBytes();
}

#endif
