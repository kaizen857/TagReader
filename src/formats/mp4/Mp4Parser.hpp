#ifndef TAGREADER_FORMATS_MP4_MP4PARSER_HPP
#define TAGREADER_FORMATS_MP4_MP4PARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_mp4
{
void ReadMp4Metadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadMp4Lyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
