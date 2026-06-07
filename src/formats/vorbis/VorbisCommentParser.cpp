#include "VorbisCommentParser.hpp"

#include "common/ParseHelpers.hpp"
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
using tagreader_common::ParseSlashNumber;
using tagreader_common::ParseUInt16;
using tagreader_common::ParseYearOnly;
using tagreader_common::ToLower;
using tagreader_core::DecodedField;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_text::DecodeTextToUtf8;
using tagreader_text::ReadLyricsFromPlainText;

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
