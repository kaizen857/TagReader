#include "VorbisCommentParser.hpp"

#include "text/TextCodec.hpp"
#include "text/TextNormalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace
{
using tagreader_core::DecodedField;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_text::DecodeTextToUtf8;
using tagreader_text::ReadLyricsFromPlainText;

uint16_t ParseUInt16(const std::string &value)
{
    if (value.empty())
    {
        return 0;
    }

    try
    {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed == 0)
        {
            return 0;
        }
        if (parsed > std::numeric_limits<uint16_t>::max())
        {
            return 0;
        }
        return static_cast<uint16_t>(parsed);
    }
    catch (...)
    {
        return 0;
    }
}

uint16_t ParseYearOnly(std::string_view text)
{
    while (!text.empty())
    {
        const unsigned char ch = static_cast<unsigned char>(text.front());
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\0')
        {
            text.remove_prefix(1);
            continue;
        }
        break;
    }

    if (text.size() < 4)
    {
        return 0;
    }

    if (!std::isdigit(static_cast<unsigned char>(text[0])) || !std::isdigit(static_cast<unsigned char>(text[1])) || !std::isdigit(static_cast<unsigned char>(text[2])) || !std::isdigit(static_cast<unsigned char>(text[3])))
    {
        return 0;
    }

    if (text.size() > 4)
    {
        const unsigned char next = static_cast<unsigned char>(text[4]);
        if (std::isdigit(next))
        {
            return 0;
        }

        const bool allowedSeparator = next == '-' || next == '/' || next == '.' || next == ' ' || next == 'T' || next == '\0';
        if (!allowedSeparator)
        {
            return 0;
        }
    }

    const uint16_t year = static_cast<uint16_t>((text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0'));
    return (year >= 1000 && year <= 9999) ? year : 0;
}

std::pair<uint16_t, uint16_t> ParseSlashNumber(const std::string &value)
{
    const auto slash = value.find('/');
    if (slash == std::string::npos)
    {
        return {ParseUInt16(value), 0};
    }
    return {ParseUInt16(value.substr(0, slash)), ParseUInt16(value.substr(slash + 1))};
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}

namespace tagreader_vorbis
{
void ReadVorbisCommentEntry(RawMetadata &metadata, std::string_view entry)
{
    const auto eq = entry.find('=');
    if (eq == std::string_view::npos)
    {
        return;
    }

    const DecodedField keyField = DecodeTextToUtf8(entry.substr(0, eq), "utf-8");
    const DecodedField valueField = DecodeTextToUtf8(entry.substr(eq + 1), "utf-8");
    if (!keyField.success || !valueField.success)
    {
        return;
    }

    const std::string key = ToLower(keyField.value);
    const std::string value = valueField.value;
    if (value.empty())
    {
        return;
    }

    if (key == "title")
    {
        if (metadata.title.empty())
            metadata.title = value;
    }
    else if (key == "artist")
    {
        if (metadata.artist.empty())
            metadata.artist = value;
    }
    else if (key == "album")
    {
        if (metadata.album.empty())
            metadata.album = value;
    }
    else if (key == "albumartist")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (key == "album_artist" || key == "album artist")
    {
        if (metadata.albumArtist.empty())
            metadata.albumArtist = value;
    }
    else if (key == "composer")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (key == "writer")
    {
        if (metadata.composer.empty())
            metadata.composer = value;
    }
    else if (key == "genre")
    {
        if (metadata.genre.empty())
            metadata.genre = value;
    }
    else if (key == "date" || key == "year")
    {
        metadata.year = metadata.year == 0 ? ParseYearOnly(value) : metadata.year;
    }
    else if (key == "tracknumber")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (key == "track" || key == "tracknum")
    {
        metadata.trackNumber = metadata.trackNumber == 0 ? ParseSlashNumber(value).first : metadata.trackNumber;
    }
    else if (key == "tracktotal" || key == "totaltracks")
    {
        return;
    }
    else if (key == "discnumber")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
    else if (key == "disc" || key == "discnum")
    {
        metadata.discNumber = metadata.discNumber == 0 ? ParseSlashNumber(value).first : metadata.discNumber;
    }
    else if (key == "disctotal" || key == "totaldiscs")
    {
        return;
    }
}

void ReadVorbisLyricsEntry(RawLyrics &lyrics, std::string_view key, std::string_view value)
{
    const DecodedField keyField = DecodeTextToUtf8(key, "utf-8");
    const DecodedField valueField = DecodeTextToUtf8(value, "utf-8");
    if (!keyField.success || !valueField.success)
    {
        return;
    }

    const std::string lowerKey = ToLower(keyField.value);
    if (lowerKey == "lyrics" || lowerKey == "unsyncedlyrics" || lowerKey == "lyric")
    {
        ReadLyricsFromPlainText(lyrics, valueField.value);
    }
    else if (lowerKey == "sylt" || lowerKey == "syncedlyrics")
    {
        ReadLyricsFromPlainText(lyrics, valueField.value);
    }
}

}
