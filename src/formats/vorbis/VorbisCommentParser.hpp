#ifndef TAGREADER_FORMATS_VORBIS_VORBISCOMMENTPARSER_HPP
#define TAGREADER_FORMATS_VORBIS_VORBISCOMMENTPARSER_HPP

#include "core/RawTagData.hpp"

#include <string_view>

namespace tagreader_vorbis
{
void ReadVorbisCommentEntry(tagreader_core::RawMetadata &metadata, std::string_view entry);
void ReadVorbisLyricsEntry(tagreader_core::RawLyrics &lyrics, std::string_view key, std::string_view value);
}

#endif
