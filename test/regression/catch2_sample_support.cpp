#include "catch2_sample_support.hpp"

#include "catch2_regression_support.hpp"

namespace tagreader_test_support
{
std::vector<std::uint8_t> OneByOnePng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

std::vector<std::uint8_t> OneByOneJpeg()
{
    return {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12, 0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29, 0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xD9};
}

std::vector<std::uint8_t> CueSheet(std::string_view title, std::string_view performer, std::string_view imageFileName, std::string_view audioFileName)
{
    std::vector<std::uint8_t> cue;
    AppendBytes(cue, "REM GENRE Test\nREM DATE 2026\nPERFORMER \"");
    AppendBytes(cue, performer);
    AppendBytes(cue, "\"\nTITLE \"");
    AppendBytes(cue, title);
    AppendBytes(cue, "\"\nFILE \"");
    AppendBytes(cue, imageFileName);
    AppendBytes(cue, "\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"");
    AppendBytes(cue, title);
    AppendBytes(cue, "\"\n    PERFORMER \"");
    AppendBytes(cue, performer);
    AppendBytes(cue, "\"\n    INDEX 01 00:00:00\nFILE \"");
    AppendBytes(cue, audioFileName);
    AppendBytes(cue, "\" MP3\n  TRACK 02 AUDIO\n    TITLE \"");
    AppendBytes(cue, title);
    AppendBytes(cue, " Bonus\"\n    INDEX 01 00:00:00\n");
    return cue;
}

std::vector<std::uint8_t> BuildId3v23Frame(std::string_view frameId, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes;
    AppendBytes(bytes, frameId);
    AppendU32BE(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> BuildId3v23Tag(const std::vector<std::uint8_t> &frames)
{
    std::vector<std::uint8_t> bytes{'I', 'D', '3', 3, 0, 0};
    const std::uint32_t size = static_cast<std::uint32_t>(frames.size());
    bytes.push_back(static_cast<std::uint8_t>((size >> 21) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 14) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 7) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(size & 0x7F));
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    return bytes;
}

bool GenerateBaseMp3(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }

    if (!HasFfmpeg())
    {
        return false;
    }

    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a libmp3lame -write_id3v1 0 -id3v2_version 0 \"" + path.string() + "\"";
    return CommandSucceeds(command);
}

bool GenerateCoverSample(const std::filesystem::path &samplePath)
{
    const std::filesystem::path basePath = samplePath.parent_path() / "base.mp3";
    const std::vector<std::uint8_t> validPng = OneByOnePng();
    std::error_code ec;
    std::filesystem::create_directories(samplePath.parent_path(), ec);
    if (ec)
    {
        return false;
    }
    std::vector<std::uint8_t> apicPayload{0};
    AppendBytes(apicPayload, "image/png");
    apicPayload.insert(apicPayload.end(), {0, 3, 0});
    apicPayload.insert(apicPayload.end(), validPng.begin(), validPng.end());
    if (!GenerateBaseMp3(basePath))
    {
        return false;
    }

    const std::vector<std::uint8_t> baseBytes = ReadBinaryFile(basePath);
    if (baseBytes.empty())
    {
        return false;
    }

    std::vector<std::uint8_t> output = BuildId3v23Tag(BuildId3v23Frame("APIC", apicPayload));
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    return WriteBinaryFile(samplePath, output);
}

bool GenerateCueSampleBundle(const std::filesystem::path &sampleRoot)
{
    const std::filesystem::path audioPath = sampleRoot / "audio.mp3";
    const std::filesystem::path imagePath = sampleRoot / "cover.jpg";
    const std::filesystem::path cuePath = sampleRoot / "album.cue";

    std::error_code ec;
    std::filesystem::create_directories(sampleRoot, ec);
    if (ec)
    {
        return false;
    }

    if (!GenerateBaseMp3(audioPath))
    {
        return false;
    }

    if (!WriteBinaryFile(imagePath, OneByOneJpeg()))
    {
        return false;
    }

    const std::vector<std::uint8_t> cue = CueSheet("cue album", "cue artist", imagePath.filename().string(), audioPath.filename().string());
    return WriteTextFile(cuePath, std::string_view(reinterpret_cast<const char *>(cue.data()), cue.size()));
}
}
