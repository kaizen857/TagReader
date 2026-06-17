#include "Id3Parser.hpp"

#include "Id3Frames.hpp"
#include "common/ParseHelpers.hpp"
#include "io/ByteReader.hpp"
#include "text/TextCodec.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace
{
using tagreader_common::ParseYearOnly;
using tagreader_core::DecodedField;
using tagreader_core::Id3TagView;
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;
using tagreader_core::ReadContext;
using tagreader_text::DecodeRawText;

} // namespace

namespace tagreader_id3
{
void ReadID3v1Metadata(ReadContext &context, RawMetadata &metadata)
{
    if (!context.input.is_open() || context.fileSize < 128)
    {
        return;
    }

    // ID3v1 只在文件尾部固定 128 字节内读取，适合做轻量补充。
    const std::vector<std::uint8_t> buffer = tagreader_io::ReadRange(context.input, context.fileSize - 128, 128);
    if (buffer.size() != 128)
    {
        return;
    }

    if (std::string_view(reinterpret_cast<const char *>(buffer.data()), 3) != "TAG")
    {
        return;
    }

    auto readField = [&](std::size_t offset, std::size_t size)
    {
        // ID3v1 has no encoding marker, so sniff raw bytes before converting to UTF-8.
        const DecodedField field = DecodeRawText(std::string_view(reinterpret_cast<const char *>(buffer.data() + offset), size));
        return field.success ? field.value : std::string{};
    };

    if (metadata.title.empty())
    {
        metadata.title = readField(3, 30);
    }
    if (metadata.artist.empty())
    {
        metadata.artist = readField(33, 30);
    }
    if (metadata.album.empty())
    {
        metadata.album = readField(63, 30);
    }
    if (metadata.year == 0)
    {
        metadata.year = ParseYearOnly(readField(93, 4));
    }
    if (metadata.genre.empty())
    {
        const auto genreIndex = static_cast<unsigned char>(buffer[127]);
        if (const std::optional<std::string_view> genre = LookupId3v1Genre(genreIndex); genre.has_value())
        {
            metadata.genre = std::string(*genre);
        }
    }

    // ID3v1.1 只有在 comment 第 29 字节为 0 且 track byte 非 0 时才表示 track number。
    if (metadata.trackNumber == 0 && buffer[125] == '\0' && buffer[126] != '\0')
    {
        metadata.trackNumber = static_cast<uint16_t>(static_cast<unsigned char>(buffer[126]));
    }
}

void ReadID3v2Metadata(ReadContext &context, RawMetadata &metadata)
{
    Id3TagView tagView{};
    if (!ReadId3TagBytes(context, tagView))
    {
        return;
    }

    if (tagView.versionMajor == 2)
    {
        if (!Id3v22TagFlagsAreSupported(tagView.flags))
        {
            return;
        }

        ReadID3v22Frames(context, metadata, tagView.bytes, tagView.cursor);
        return;
    }

    ReadID3v23Or24Frames(context, metadata, tagView.bytes, tagView.versionMajor, tagView.tagUnsync, tagView.cursor, tagView.limit);
}


void ReadID3Lyrics(ReadContext &context, RawLyrics &lyrics)
{
    Id3TagView tagView{};
    if (!ReadId3TagBytes(context, tagView))
    {
        return;
    }

    if (tagView.versionMajor == 2)
    {
        if (!Id3v22TagFlagsAreSupported(tagView.flags))
        {
            return;
        }

        ReadID3v22LyricsFrames(context, lyrics, tagView.bytes, tagView.cursor);
        return;
    }

    ReadID3v23Or24LyricsFrames(context, lyrics, tagView.bytes, tagView.versionMajor, tagView.tagUnsync, tagView.cursor, tagView.limit);
}


} // namespace tagreader_id3
