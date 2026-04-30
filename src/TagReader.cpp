#include "TagReader.hpp"

#include <stdexcept>

namespace
{
[[noreturn]] void NotImplemented(const char *name)
{
    throw std::logic_error(std::string(name) + " is not implemented yet");
}
} // namespace

MusicTag TagReader::Read(const std::filesystem::path &filePath)
{
    ValidatePath(filePath);

    const ReadContext context = OpenContext(filePath);
    DetectStream(context);

    const RawMediaInfo mediaInfo = ReadMediaInfo(context);
    const RawMetadata metadata = ReadMetadata(context);
    const RawLyrics lyrics = ReadLyrics(context);

    return BuildMusicTag(mediaInfo, metadata, lyrics);
}

void TagReader::ValidatePath(const std::filesystem::path &filePath)
{
    if (filePath.empty())
    {
        throw std::invalid_argument("file path is empty");
    }

    NotImplemented("TagReader::ValidatePath");
}

TagReader::ReadContext TagReader::OpenContext(const std::filesystem::path &filePath)
{
    NotImplemented("TagReader::OpenContext");
    return ReadContext{filePath};
}

void TagReader::DetectStream(const ReadContext &context)
{
    (void)context;
    NotImplemented("TagReader::DetectStream");
}

TagReader::RawMediaInfo TagReader::ReadMediaInfo(const ReadContext &context)
{
    (void)context;
    NotImplemented("TagReader::ReadMediaInfo");
    return {};
}

TagReader::RawMetadata TagReader::ReadMetadata(const ReadContext &context)
{
    (void)context;
    NotImplemented("TagReader::ReadMetadata");
    return {};
}

TagReader::RawLyrics TagReader::ReadLyrics(const ReadContext &context)
{
    (void)context;
    NotImplemented("TagReader::ReadLyrics");
    return {};
}

TagReader::DecodedField TagReader::NormalizeText(std::string_view value)
{
    (void)value;
    NotImplemented("TagReader::NormalizeText");
    return {};
}

MusicTag TagReader::BuildMusicTag(const RawMediaInfo &mediaInfo, const RawMetadata &metadata, const RawLyrics &lyrics)
{
    (void)mediaInfo;
    (void)metadata;
    (void)lyrics;
    NotImplemented("TagReader::BuildMusicTag");
    return {};
}
