#include "flac_malformed_metadata_support.hpp"

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"

#include <libavutil/log.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace flac_malformed_test
{
namespace
{
using tagreader_test_support::AppendBytes;
using tagreader_test_support::AppendU24BE;
using tagreader_test_support::AppendU32BE;
using tagreader_test_support::AppendU32LE;
using tagreader_test_support::Bytes;
using tagreader_test_support::HasFfmpeg;
using tagreader_test_support::PrepareCleanDirectory;
using tagreader_test_support::ReadBinaryFile;
using tagreader_test_support::WriteBinaryFile;

std::vector<std::uint8_t> OneByOnePng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

std::vector<std::uint8_t> VorbisCommentPayload(std::initializer_list<std::string_view> comments)
{
    constexpr std::string_view kVendor = "tagreader-flac-malformed-test";
    std::vector<std::uint8_t> payload;
    AppendU32LE(payload, static_cast<std::uint32_t>(kVendor.size()));
    AppendBytes(payload, kVendor);
    AppendU32LE(payload, static_cast<std::uint32_t>(comments.size()));
    for (std::string_view comment : comments)
    {
        AppendU32LE(payload, static_cast<std::uint32_t>(comment.size()));
        AppendBytes(payload, comment);
    }
    return payload;
}

std::vector<std::uint8_t> MalformedVorbisCommentPayload()
{
    std::vector<std::uint8_t> payload;
    AppendU32LE(payload, 8);
    AppendBytes(payload, "bad");
    return payload;
}

std::vector<std::uint8_t> FlacPicturePayload(const std::vector<std::uint8_t> &imageBytes)
{
    constexpr std::string_view kPngMime = "image/png";
    std::vector<std::uint8_t> payload;
    AppendU32BE(payload, 3);
    AppendU32BE(payload, static_cast<std::uint32_t>(kPngMime.size()));
    AppendBytes(payload, kPngMime);
    AppendU32BE(payload, 0);
    AppendU32BE(payload, 1);
    AppendU32BE(payload, 1);
    AppendU32BE(payload, 32);
    AppendU32BE(payload, 0);
    AppendU32BE(payload, static_cast<std::uint32_t>(imageBytes.size()));
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return payload;
}

bool GenerateFlacSample(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "failed to create FLAC sample directory: " << ec.message() << '\n';
        return false;
    }

    if (!HasFfmpeg())
    {
        std::cerr << "ffmpeg CLI not found; FlacMalformed Catch2 tests require generated audio\n";
        return false;
    }

    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a flac \"" + path.string() + "\"";
    return tagreader_test_support::CommandSucceeds(command);
}

bool AppendFlacMetadataBlock(std::vector<std::uint8_t> &output, std::uint8_t blockType, bool lastBlock, const std::vector<std::uint8_t> &payload)
{
    if (payload.size() > 0xFFFFFFU)
    {
        std::cerr << "FLAC metadata payload too large\n";
        return false;
    }
    output.push_back(static_cast<std::uint8_t>((lastBlock ? 0x80 : 0x00) | (blockType & 0x7F)));
    AppendU24BE(output, static_cast<std::uint32_t>(payload.size()));
    output.insert(output.end(), payload.begin(), payload.end());
    return true;
}

bool ReplaceMetadataTail(const std::filesystem::path &basePath,
                         const std::filesystem::path &outputPath,
                         const std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> &tailBlocks)
{
    const std::vector<std::uint8_t> data = ReadBinaryFile(basePath);
    if (data.size() < 42 || std::string_view(reinterpret_cast<const char *>(data.data()), 4) != "fLaC")
    {
        std::cerr << "failed to read base FLAC sample: " << basePath.string() << '\n';
        return false;
    }

    std::size_t cursor = 4;
    std::size_t audioStart = data.size();
    bool foundStreamInfo = false;
    std::vector<std::uint8_t> output{'f', 'L', 'a', 'C'};

    while (cursor + 4 <= data.size())
    {
        const bool lastBlock = (data[cursor] & 0x80) != 0;
        const std::uint8_t blockType = data[cursor] & 0x7F;
        const std::uint32_t blockSize = tagreader_test_support::ReadU24BE(data, cursor + 1);
        const std::size_t blockPayload = cursor + 4;
        const std::size_t blockEnd = blockPayload + blockSize;
        if (blockEnd > data.size())
        {
            break;
        }

        if (blockType == 0)
        {
            foundStreamInfo = true;
            if (!AppendFlacMetadataBlock(output, blockType, false, std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(blockPayload), data.begin() + static_cast<std::ptrdiff_t>(blockEnd))))
            {
                return false;
            }
        }

        cursor = blockEnd;
        if (lastBlock)
        {
            audioStart = cursor;
            break;
        }
    }

    if (!foundStreamInfo || audioStart > data.size() || tailBlocks.empty())
    {
        std::cerr << "base FLAC sample has no complete STREAMINFO block or no replacement tail\n";
        return false;
    }

    for (std::size_t i = 0; i < tailBlocks.size(); ++i)
    {
        if (!AppendFlacMetadataBlock(output, tailBlocks[i].first, i + 1 == tailBlocks.size(), tailBlocks[i].second))
        {
            return false;
        }
    }

    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(audioStart), data.end());
    return WriteBinaryFile(outputPath, output);
}

bool Expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "expectation failed: " << message << '\n';
        return false;
    }
    return true;
}
}

std::filesystem::path MakeCaseRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_flac_malformed_metadata_catch2" / std::string(caseName);
}

bool PrepareCaseRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

bool RunLaterValidVorbisBlockSurvives(const std::filesystem::path &root)
{
    const std::filesystem::path basePath = root / "base.flac";
    const std::filesystem::path samplePath = root / "malformed-then-valid.flac";
    const std::filesystem::path coverExportDir = root / "covers-valid-title";

    if (!GenerateFlacSample(basePath) || !ReplaceMetadataTail(basePath, samplePath, {{4, MalformedVorbisCommentPayload()}, {4, VorbisCommentPayload({"TITLE=later-valid-flac-title", "ARTIST=later-valid-flac-artist"})}}))
    {
        return false;
    }

    const MusicTag tag = TagReader::Read(samplePath, coverExportDir);
    return Expect(tag.title() == "later-valid-flac-title", "later valid FLAC Vorbis comment block should provide title") &&
           Expect(tag.artist() == "later-valid-flac-artist", "later valid FLAC Vorbis comment block should provide artist");
}

bool RunFlacPictureCoverCacheFailurePropagates(const std::filesystem::path &root)
{
    constexpr std::string_view kOneByOnePngSha256 = "4ff6ab670a58c14270e034e2090d9a432caa263a14e0a25785386b0c12f880b5";
    const std::filesystem::path basePath = root / "base-picture.flac";
    const std::filesystem::path samplePath = root / "picture.flac";
    const std::filesystem::path coverExportDir = root / "covers-picture";
    const std::filesystem::path expectedCoverPath = coverExportDir / "artwork" / std::string(kOneByOnePngSha256.substr(0, 2)) / (std::string(kOneByOnePngSha256.substr(2)) + ".png");

    if (!GenerateFlacSample(basePath) || !ReplaceMetadataTail(basePath, samplePath, {{6, FlacPicturePayload(OneByOnePng())}}))
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(expectedCoverPath.parent_path(), ec);
    if (ec || !WriteBinaryFile(expectedCoverPath, Bytes("polluted cover cache entry")))
    {
        return false;
    }

    MusicTag result;
    try
    {
        result = TagReader::Read(samplePath, coverExportDir);
    }
    catch (const std::exception &ex)
    {
        return Expect(false, std::string("FLAC PICTURE polluted cache should be auto-fixed, not throw: ") + ex.what());
    }

    const bool coverPathPresent = Expect(!result.coverPath().empty(), "polluted cache should be replaced with valid cover");
    const bool coverPathExpected = Expect(result.coverPath() == expectedCoverPath, "cover path should match expected SHA-256 cache path");
    const bool validCoverExists = Expect(std::filesystem::is_regular_file(expectedCoverPath, ec), "valid cover should exist after auto-fix");
    return coverPathPresent && coverPathExpected && validCoverExists;
}
}
