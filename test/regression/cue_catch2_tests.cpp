#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "TagReader.hpp"
#include "../../src/formats/cue/CueTextLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <system_error>

namespace
{
bool EnsureCleanRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

bool WriteBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    return tagreader_test_support::WriteBinaryFile(path, bytes);
}

std::vector<std::uint8_t> Utf8Bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> Utf16LeBytes(std::u16string_view text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + text.size() * 2);
    bytes.push_back(0xFF);
    bytes.push_back(0xFE);
    for (char16_t ch : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(ch & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((ch >> 8) & 0xFF));
    }
    return bytes;
}

std::vector<std::uint8_t> Utf16BeBytes(std::u16string_view text)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + text.size() * 2);
    bytes.push_back(0xFE);
    bytes.push_back(0xFF);
    for (char16_t ch : text)
    {
        bytes.push_back(static_cast<std::uint8_t>((ch >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(ch & 0xFF));
    }
    return bytes;
}
}

TEST_CASE("cue sample helpers create temp-only artifacts", "[cue]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_sample_helpers");
    REQUIRE(EnsureCleanRoot(root));
    REQUIRE(tagreader_test_support::GenerateCueSampleBundle(root));
    REQUIRE(std::filesystem::exists(root / "album.cue"));
    REQUIRE(std::filesystem::exists(root / "cover.jpg"));
    REQUIRE(std::filesystem::exists(root / "audio.mp3"));
}

TEST_CASE("cue text loader decodes utf8 and bom text", "[cue][encoding]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_encoding_utf8");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path utf8Path = root / "utf8.cue";
    REQUIRE(WriteBytes(utf8Path, Utf8Bytes("TITLE \"plain\"\n")));
    REQUIRE(tagreader_cue::LoadCueTextUtf8(utf8Path).has_value());

    const std::filesystem::path utf8BomPath = root / "utf8-bom.cue";
    const std::vector<std::uint8_t> utf8BomBytes{0xEF, 0xBB, 0xBF, 'T', 'I', 'T', 'L', 'E', ' ', '"', 'b', 'o', 'm', '"', '\n'};
    REQUIRE(WriteBytes(utf8BomPath, utf8BomBytes));
    const auto utf8BomDecoded = tagreader_cue::LoadCueTextUtf8(utf8BomPath);
    REQUIRE(utf8BomDecoded.has_value());
    REQUIRE(*utf8BomDecoded == "TITLE \"bom\"");

    const std::vector<std::uint8_t> utf16LeBytes = Utf16LeBytes(u"TITLE \"le\"\n");
    REQUIRE(WriteBytes(root / "utf16le.cue", utf16LeBytes));
    const auto utf16LeDecoded = tagreader_cue::LoadCueTextUtf8(root / "utf16le.cue");
    REQUIRE(utf16LeDecoded.has_value());
    REQUIRE(*utf16LeDecoded == "TITLE \"le\"");

    const std::vector<std::uint8_t> utf16BeBytes = Utf16BeBytes(u"TITLE \"be\"\n");
    REQUIRE(WriteBytes(root / "utf16be.cue", utf16BeBytes));
    const auto utf16BeDecoded = tagreader_cue::LoadCueTextUtf8(root / "utf16be.cue");
    REQUIRE(utf16BeDecoded.has_value());
    REQUIRE(*utf16BeDecoded == "TITLE \"be\"");

    REQUIRE(tagreader_cue::LoadCueTextUtf8(utf8Path).has_value());
}

TEST_CASE("cue text loader rejects oversize and undecodable text", "[cue][encoding]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_encoding_invalid");
    REQUIRE(EnsureCleanRoot(root));

    const std::string oversize(4ULL * 1024ULL * 1024ULL + 1ULL, 'a');
    REQUIRE(WriteBytes(root / "oversize.cue", Utf8Bytes(oversize)));
    REQUIRE_FALSE(tagreader_cue::LoadCueTextUtf8(root / "oversize.cue").has_value());

    REQUIRE(WriteBytes(root / "bad.cue", std::vector<std::uint8_t>{0xFF, 0xFE, 0x61}));
    REQUIRE_FALSE(tagreader_cue::LoadCueTextUtf8(root / "bad.cue").has_value());

    const std::filesystem::path directoryCue = root / "directory.cue";
    REQUIRE(std::filesystem::create_directory(directoryCue));
    REQUIRE_FALSE(tagreader_cue::LoadCueTextUtf8(directoryCue).has_value());
}

TEST_CASE("cue text loader accepts latin1 fallback text", "[cue][encoding]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_encoding_latin1");
    REQUIRE(EnsureCleanRoot(root));

    const std::vector<std::uint8_t> latin1Bytes{'T', 'I', 'T', 'L', 'E', ' ', '"', 'c', 'a', 'f', 0xE9, '"', '\n'};
    REQUIRE(WriteBytes(root / "latin1.cue", latin1Bytes));
    const auto latin1Decoded = tagreader_cue::LoadCueTextUtf8(root / "latin1.cue");
    REQUIRE(latin1Decoded.has_value());
    REQUIRE(*latin1Decoded == "TITLE \"café\"");
    REQUIRE(tagreader_test_support::WriteTextFile(root / "latin1-note.txt", *latin1Decoded));
}
