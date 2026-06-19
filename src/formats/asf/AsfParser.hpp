#ifndef TAGREADER_FORMATS_ASF_ASFPARSER_HPP
#define TAGREADER_FORMATS_ASF_ASFPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_asf
{
void ReadAsfMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadAsfLyrics(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics);
}

#endif
