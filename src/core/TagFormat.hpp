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
    OggOpus,
    Mp4,
    Ape,
    RiffWav,
    Aiff,
    Dsf,
    Dff,
    Asf,
    Matroska,
    RawId3v2,
    RawVorbisComment,
    RawMp4Ilst,
    RawApeV2,
};
}

#endif
