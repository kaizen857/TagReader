#ifndef TAGREADER_FORMATS_AIFF_AIFFPARSER_HPP
#define TAGREADER_FORMATS_AIFF_AIFFPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_aiff
{
void ReadAiffMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
}

#endif
