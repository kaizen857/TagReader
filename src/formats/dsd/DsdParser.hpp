#ifndef TAGREADER_FORMATS_DSD_DSDPARSER_HPP
#define TAGREADER_FORMATS_DSD_DSDPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_dsd
{
void ReadDsfMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
void ReadDffMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
}

#endif
