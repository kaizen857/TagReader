#ifndef TAGREADER_TEXT_TEXTNORMALIZE_HPP
#define TAGREADER_TEXT_TEXTNORMALIZE_HPP

#include "core/RawTagData.hpp"

#include <string_view>

namespace tagreader_text
{
void NormalizeMetadata(tagreader_core::RawMetadata &metadata);
void NormalizeLyrics(tagreader_core::RawLyrics &lyrics);
void ReadLyricsFromPlainText(tagreader_core::RawLyrics &lyrics, std::string_view text);
}

#endif
