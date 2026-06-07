#include "media/ContainerDetector.hpp"

#include "common/ParseHelpers.hpp"
#include "io/ByteReader.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace tagreader_media
{
namespace
{
using tagreader_common::ToLower;

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> needles)
{
    for (const auto needle : needles)
    {
        if (value.find(needle) != std::string_view::npos)
        {
            return true;
        }
    }
    return false;
}

bool HasId3v1Footer(tagreader_core::ReadContext &context)
{
    if (context.fileSize < 128)
    {
        return false;
    }

    const std::vector<uint8_t> footer = tagreader_io::ReadRange(context.input, context.fileSize - 128, 3);
    return footer.size() == 3 && std::memcmp(footer.data(), "TAG", 3) == 0;
}

bool HasApeFooter(tagreader_core::ReadContext &context, uint32_t &tagSize, uint32_t &itemCount, uint32_t &flags)
{
    if (context.fileSize < 32)
    {
        return false;
    }

    const std::vector<uint8_t> footer = tagreader_io::ReadRange(context.input, context.fileSize - 32, 32);
    if (footer.size() != 32 || std::memcmp(footer.data(), "APETAGEX", 8) != 0)
    {
        return false;
    }

    const uint32_t version = tagreader_io::ReadLE32(footer.data() + 8);
    if (version < 2000)  // APEv1 — skip
    {
        return false;
    }

    tagSize = tagreader_io::ReadLE32(footer.data() + 12);
    itemCount = tagreader_io::ReadLE32(footer.data() + 16);
    flags = tagreader_io::ReadLE32(footer.data() + 20);

    return true;
}
}

tagreader_core::DetectedContainer ContainerFromTagFormat(tagreader_core::TagFormat tagFormat)
{
    using tagreader_core::DetectedContainer;
    using tagreader_core::TagFormat;

    switch (tagFormat)
    {
    case TagFormat::Id3v1:
    case TagFormat::Id3v2:
        return DetectedContainer::Mp3;
    case TagFormat::Flac:
    case TagFormat::VorbisComment:
        return DetectedContainer::Flac;
    case TagFormat::OggVorbis:
        return DetectedContainer::OggVorbis;
    case TagFormat::Mp4:
        return DetectedContainer::Mp4;
    case TagFormat::Ape:
        return DetectedContainer::Ape;
    case TagFormat::Unknown:
        return DetectedContainer::Unknown;
    }

    return DetectedContainer::Unknown;
}

tagreader_core::TagFormat DetectTagFormat(tagreader_core::ReadContext &context)
{
    using tagreader_core::TagFormat;

    if (!context.input.is_open())
    {
        return TagFormat::Unknown;
    }

    // APE footer takes priority over ID3 — ensures MP3+APE files use APE metadata.
    if (context.fileSize >= 32)
    {
        uint32_t apeTagSize = 0;
        uint32_t apeItemCount = 0;
        uint32_t apeFlags = 0;
        if (HasApeFooter(context, apeTagSize, apeItemCount, apeFlags))
        {
            return TagFormat::Ape;
        }
    }

    const std::vector<uint8_t> header = tagreader_io::ReadRange(context.input, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(context.fileSize, 12)));
    if (header.size() >= 3 && std::memcmp(header.data(), "ID3", 3) == 0)
    {
        return TagFormat::Id3v2;
    }
    if (header.size() >= 4 && std::string_view(reinterpret_cast<const char *>(header.data()), 4) == "fLaC")
    {
        return TagFormat::Flac;
    }
    if (header.size() >= 4 && std::string_view(reinterpret_cast<const char *>(header.data()), 4) == "OggS")
    {
        return TagFormat::OggVorbis;
    }
    if (header.size() >= 8 && std::string_view(reinterpret_cast<const char *>(header.data() + 4), 4) == "ftyp")
    {
        return TagFormat::Mp4;
    }
    if (HasId3v1Footer(context))
    {
        return TagFormat::Id3v1;
    }

    const std::string container = ToLower(context.containerName);
    if (ContainsAny(container, {"mp4", "mov", "m4"}))
    {
        return TagFormat::Mp4;
    }
    if (ContainsAny(container, {"ogg", "vorbis"}))
    {
        return TagFormat::OggVorbis;
    }
    if (container.find("flac") != std::string::npos)
    {
        return TagFormat::Flac;
    }
    if (ContainsAny(container, {"mp3", "mpeg"}))
    {
        return TagFormat::Id3v2;
    }
    if (ContainsAny(container, {"ape", "mpc", "mpc8", "wv", "tak", "tta"}))
    {
        return TagFormat::Ape;
    }

    return TagFormat::Unknown;
}
}
