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

bool HasAsciiAt(const std::vector<uint8_t> &bytes, std::size_t offset, std::string_view magic)
{
    if (offset > bytes.size() || magic.size() > bytes.size() - offset)
    {
        return false;
    }

    return std::memcmp(bytes.data() + offset, magic.data(), magic.size()) == 0;
}

bool ContainsAscii(const std::vector<uint8_t> &bytes, std::string_view magic)
{
    return std::search(bytes.begin(), bytes.end(), magic.begin(), magic.end(),
                       [](uint8_t lhs, char rhs)
                       { return lhs == static_cast<uint8_t>(rhs); }) != bytes.end();
}

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
    case TagFormat::OggOpus:
        return DetectedContainer::OggOpus;
    case TagFormat::Mp4:
        return DetectedContainer::Mp4;
    case TagFormat::Ape:
        return DetectedContainer::Ape;
    case TagFormat::RiffWav:
        return DetectedContainer::RiffWav;
    case TagFormat::Aiff:
        return DetectedContainer::Aiff;
    case TagFormat::Dsf:
        return DetectedContainer::Dsf;
    case TagFormat::Dff:
        return DetectedContainer::Dff;
    case TagFormat::Asf:
        return DetectedContainer::Asf;
    case TagFormat::Matroska:
        return DetectedContainer::Matroska;
    case TagFormat::RawId3v2:
    case TagFormat::RawVorbisComment:
    case TagFormat::RawMp4Ilst:
    case TagFormat::RawApeV2:
        return DetectedContainer::RawTagSource;
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

    constexpr std::uintmax_t kHeaderProbeBytes = 64;
    const std::vector<uint8_t> header = tagreader_io::ReadRange(context.input, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(context.fileSize, kHeaderProbeBytes)));
    if (HasAsciiAt(header, 0, "ID3"))
    {
        return TagFormat::Id3v2;
    }
    if (HasAsciiAt(header, 0, "fLaC"))
    {
        return TagFormat::Flac;
    }
    if (HasAsciiAt(header, 0, "OggS"))
    {
        if (ContainsAscii(header, "OpusHead"))
        {
            return TagFormat::OggOpus;
        }
        return TagFormat::OggVorbis;
    }
    if (HasAsciiAt(header, 4, "ftyp"))
    {
        return TagFormat::Mp4;
    }
    if (HasAsciiAt(header, 0, "RIFF") && HasAsciiAt(header, 8, "WAVE"))
    {
        return TagFormat::RiffWav;
    }
    if (HasAsciiAt(header, 0, "FORM") && (HasAsciiAt(header, 8, "AIFF") || HasAsciiAt(header, 8, "AIFC")))
    {
        return TagFormat::Aiff;
    }
    if (HasAsciiAt(header, 0, "DSD "))
    {
        return TagFormat::Dsf;
    }
    if (HasAsciiAt(header, 0, "FRM8") && HasAsciiAt(header, 12, "DSD "))
    {
        return TagFormat::Dff;
    }
    constexpr uint8_t kAsfHeaderGuid[] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                          0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    if (header.size() >= sizeof(kAsfHeaderGuid) && std::memcmp(header.data(), kAsfHeaderGuid, sizeof(kAsfHeaderGuid)) == 0)
    {
        return TagFormat::Asf;
    }
    if (header.size() >= 4 && header[0] == 0x1A && header[1] == 0x45 && header[2] == 0xDF && header[3] == 0xA3)
    {
        return TagFormat::Matroska;
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
    if (container.find("opus") != std::string::npos)
    {
        return TagFormat::OggOpus;
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
    if (ContainsAny(container, {"wav", "wave"}))
    {
        return TagFormat::RiffWav;
    }
    if (ContainsAny(container, {"aiff", "aifc"}))
    {
        return TagFormat::Aiff;
    }
    if (container.find("dsf") != std::string::npos)
    {
        return TagFormat::Dsf;
    }
    if (ContainsAny(container, {"dff", "dsdiff"}))
    {
        return TagFormat::Dff;
    }
    if (ContainsAny(container, {"asf", "wma"}))
    {
        return TagFormat::Asf;
    }
    if (ContainsAny(container, {"matroska", "webm", "mka"}))
    {
        return TagFormat::Matroska;
    }

    return TagFormat::Unknown;
}
}
