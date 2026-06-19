#ifndef TAGREADER_FORMATS_ID3_ID3PARSER_HPP
#define TAGREADER_FORMATS_ID3_ID3PARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

#include <cstdint>
#include <vector>

namespace tagreader_id3
{
void ReadID3v1Metadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadID3v2Metadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadID3v2MetadataFromBytes(tagreader_core::ReadContext &context, const std::vector<uint8_t> &tagBytes, tagreader_core::RawMetadata &metadata);
void ReadID3Lyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
