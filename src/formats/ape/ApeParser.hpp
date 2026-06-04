#ifndef TAGREADER_FORMATS_APE_APEPARSER_HPP
#define TAGREADER_FORMATS_APE_APEPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_ape
{
void ReadApeMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadApeLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
