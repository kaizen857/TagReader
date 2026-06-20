#ifndef TAGREADER_FORMATS_CUE_CUETIMING_HPP
#define TAGREADER_FORMATS_CUE_CUETIMING_HPP

#include "formats/cue/CueParser.hpp"

#include "Tag.hpp"

#include <vector>

namespace tagreader_cue
{
bool ApplyCueTiming(const CueFile &file, std::vector<MusicTag> &tags);
}

#endif
