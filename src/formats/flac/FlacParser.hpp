#ifndef TAGREADER_FORMATS_FLAC_FLACPARSER_HPP
#define TAGREADER_FORMATS_FLAC_FLACPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_flac
{
void ReadFlacMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadFlacLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
