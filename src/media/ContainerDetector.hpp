#ifndef TAGREADER_MEDIA_CONTAINERDETECTOR_HPP
#define TAGREADER_MEDIA_CONTAINERDETECTOR_HPP

#include "core/ReadContext.hpp"
#include "core/TagFormat.hpp"

namespace tagreader_media
{
tagreader_core::TagFormat DetectTagFormat(tagreader_core::ReadContext &context);
tagreader_core::DetectedContainer ContainerFromTagFormat(tagreader_core::TagFormat tagFormat);
}

#endif
