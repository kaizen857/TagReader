#include "formats/flac/FlacPicture.hpp"

#include "cover/CoverCache.hpp"
#include "io/ByteReader.hpp"
#include "profiling/Profiling.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace
{
using tagreader_cover::WriteCoverAsPng;
using tagreader_io::ByteCursor;

constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}
}

namespace tagreader_flac
{
void ReadFlacPictureEntry(tagreader_core::ReadContext &context, tagreader_core::RawMetadata &metadata, const uint8_t *pictureData, std::size_t pictureSize)
{
    TAGREADER_PROFILE_FUNCTION();
    
    if (pictureData == nullptr || pictureSize < 32)
    {
        return;
    }
    if (!metadata.coverPath.empty())
    {
        return;
    }

    ByteCursor cursor(pictureData, pictureSize);
    const std::optional<std::uint32_t> pictureType = cursor.readU32Be();
    const std::optional<std::uint32_t> mimeLen = cursor.readU32Be();
    if (!pictureType.has_value() || !mimeLen.has_value())
    {
        return;
    }

    const std::optional<std::span<const uint8_t>> mimeBytes = cursor.readBytes(*mimeLen);
    if (!mimeBytes.has_value())
    {
        return;
    }
    const std::string mime = ToLower(std::string(reinterpret_cast<const char *>(mimeBytes->data()), mimeBytes->size()));

    const std::optional<std::uint32_t> descLen = cursor.readU32Be();
    if (!descLen.has_value() || !cursor.skip(*descLen))
    {
        return;
    }
    if (!cursor.skip(4) || !cursor.skip(4) || !cursor.skip(4) || !cursor.skip(4))
    {
        return;
    }

    const std::optional<std::uint32_t> picDataLen = cursor.readU32Be();
    if (!picDataLen.has_value())
    {
        return;
    }
    if (picDataLen > kMaxCoverInputBytes)
        return;

    const std::optional<std::span<const uint8_t>> imageBytes = cursor.readBytes(*picDataLen);
    if (!imageBytes.has_value())
    {
        return;
    }

    if (mime == "-->")
    {
        return;
    }

    if (*pictureType != 3)
    {
        return;
    }

    const std::filesystem::path coverPath = WriteCoverAsPng(context.coverExportDir, imageBytes->data(), imageBytes->size());
    if (!coverPath.empty())
    {
        metadata.coverPath = coverPath;
    }
}
}
