#ifndef TAGREADER_MEDIA_MEDIAINFOREADER_HPP
#define TAGREADER_MEDIA_MEDIAINFOREADER_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

namespace tagreader_media
{
void DetectStream(tagreader_core::ReadContext &context);
tagreader_core::RawMediaInfo ReadMediaInfo(const tagreader_core::ReadContext &context);
}

#endif
