#ifndef TAGREADER_FORMATS_OPUS_OPUSPARSER_HPP
#define TAGREADER_FORMATS_OPUS_OPUSPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_opus
{
void ReadOggOpusMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadOggOpusLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
