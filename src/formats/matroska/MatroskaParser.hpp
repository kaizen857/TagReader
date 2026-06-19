#ifndef TAGREADER_FORMATS_MATROSKA_MATROSKAPARSER_HPP
#define TAGREADER_FORMATS_MATROSKA_MATROSKAPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_matroska
{
void ReadMatroskaMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
}

#endif
