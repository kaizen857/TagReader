#ifndef TAGREADER_FORMATS_OGG_VORBIS_OGGVORBISPARSER_HPP
#define TAGREADER_FORMATS_OGG_VORBIS_OGGVORBISPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_ogg_vorbis
{
void ReadOggVorbisMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadOggVorbisLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
