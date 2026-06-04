#ifndef TAGREADER_CORE_TAGFORMAT_HPP
#define TAGREADER_CORE_TAGFORMAT_HPP

namespace tagreader_core
{
enum class TagFormat
{
    Unknown,
    Id3v1,
    Id3v2,
    VorbisComment,
    Flac,
    OggVorbis,
    Mp4,
    Ape,
};
}

#endif
