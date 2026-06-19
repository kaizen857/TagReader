#ifndef TAGREADER_FORMATS_FLAC_FLACPICTURE_HPP
#define TAGREADER_FORMATS_FLAC_FLACPICTURE_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

#include <cstddef>
#include <cstdint>

namespace tagreader_flac
{
void ReadFlacPictureEntry(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize);
}

#endif
