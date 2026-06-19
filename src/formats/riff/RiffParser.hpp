#ifndef TAGREADER_FORMATS_RIFF_RIFFPARSER_HPP
#define TAGREADER_FORMATS_RIFF_RIFFPARSER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_riff
{
void ReadRiffWavMetadata(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata);
}

#endif
