#ifndef TAGREADER_FORMATS_ID3_ID3FRAMES_HPP
#define TAGREADER_FORMATS_ID3_ID3FRAMES_HPP

#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tagreader_id3
{
bool ReadId3TagBytes(tagreader_core::ReadContext &context, tagreader_core::Id3TagView &tagView);
bool Id3v22TagFlagsAreSupported(uint8_t tagFlags);
std::optional<std::string_view> LookupId3v1Genre(unsigned char genreIndex);
void ReadID3v22Frames(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, std::size_t cursor);
void ReadID3v23Or24Frames(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit);
void ReadID3v22LyricsFrames(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, std::size_t cursor);
void ReadID3v23Or24LyricsFrames(tagreader_core::ReadContext &context, tagreader_core::RawLyrics &lyrics, const std::vector<uint8_t> &tagBytes, uint8_t versionMajor, bool tagUnsync, std::size_t cursor, std::size_t limit);
}

#endif
