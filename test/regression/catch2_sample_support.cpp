#include "catch2_sample_support.hpp"

#include "catch2_regression_support.hpp"

namespace tagreader_test_support
{
std::vector<std::uint8_t> OneByOnePng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
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
}
