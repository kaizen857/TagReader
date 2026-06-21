#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "TagReader.hpp"
#include "../../src/cover/CoverCache.hpp"
#include "../../src/formats/cue/CueParser.hpp"
#include "../../src/formats/cue/CueTextLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits>
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

bool WriteCoverImage(const std::filesystem::path &path, bool jpeg)
{
    return WriteBytes(path, jpeg ? tagreader_test_support::OneByOneJpeg() : tagreader_test_support::OneByOnePng());
}

bool ExpectPathEqual(const std::filesystem::path &left, const std::filesystem::path &right)
{
    return std::filesystem::weakly_canonical(left) == std::filesystem::weakly_canonical(right);
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

std::string LongText(std::size_t size, char ch)
{
    return std::string(size, ch);
}

std::string BasicCueSheet()
{
    return "REM GENRE Rock\n"
           "REM DATE 2026\n"
           "REM YEAR 2026\n"
           "REM DISCNUMBER 2\n"
           "TITLE \"Album\"\n"
           "PERFORMER \"Artist\"\n"
           "FILE \"disc one.flac\" FLAC\n"
           "  TRACK 01 AUDIO\n"
           "    TITLE \"Intro\"\n"
           "    PERFORMER \"Band\"\n"
           "    SONGWRITER \"Writer\"\n"
           "    INDEX 01 00:00:00\n"
           "    INDEX 02 00:01:12\n";
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

TEST_CASE("cue parser handles single file and selected rem fields", "[cue][parser]")
{
    const auto parsed = tagreader_cue::ParseCueSheet(BasicCueSheet());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->global.title == "Album");
    REQUIRE(parsed->global.performer == "Artist");
    REQUIRE(parsed->global.genre == "Rock");
    REQUIRE(parsed->global.date == "2026");
    REQUIRE(parsed->global.year == "2026");
    REQUIRE(parsed->global.discNumber == "2");
    REQUIRE(parsed->files.size() == 1);
    REQUIRE(parsed->files.front().name == "disc one.flac");
    REQUIRE(parsed->files.front().format == "FLAC");
    REQUIRE(parsed->files.front().tracks.size() == 1);
    REQUIRE(parsed->files.front().tracks.front().title == "Intro");
    REQUIRE(parsed->files.front().tracks.front().performer == "Band");
    REQUIRE(parsed->files.front().tracks.front().songwriter == "Writer");
    REQUIRE(parsed->files.front().tracks.front().indexes.size() == 2);
    REQUIRE(parsed->files.front().tracks.front().indexes.front().frame == 0);
    REQUIRE(parsed->files.front().tracks.front().indexes.back().frame == 12);
}

TEST_CASE("cue parser handles lowercase mixedcase and unknown commands", "[cue][parser]")
{
    const std::string cue = "  reM genre Jazz\n"
                            "title \"album\"\n"
                            "unknown something\n"
                            "FiLe \"set.wav\" wave\n"
                            "  TrAcK 01 Audio\n"
                            "    TiTlE \"song\"\n"
                            "    PeRfOrMeR \"artist\"\n"
                            "    Index 01 00:00:00\n";
    const auto parsed = tagreader_cue::ParseCueSheet(cue);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->global.genre == "Jazz");
    REQUIRE(parsed->files.size() == 1);
    REQUIRE(parsed->files.front().tracks.size() == 1);
    REQUIRE(parsed->files.front().tracks.front().title == "song");
    REQUIRE(parsed->files.front().tracks.front().performer == "artist");
}

TEST_CASE("cue parser handles multi file and quoted fields", "[cue][parser]")
{
    const std::string cue = "TITLE \"multi album\"\n"
                            "FILE \"disc a.flac\" FLAC\n"
                            "  TRACK 01 AUDIO\n"
                            "    TITLE \"A track\"\n"
                            "    INDEX 01 00:00:00\n"
                            "FILE \"disc b.flac\" FLAC\n"
                            "  TRACK 02 AUDIO\n"
                            "    TITLE \"B track\"\n"
                            "    INDEX 01 00:10:00\n";
    const auto parsed = tagreader_cue::ParseCueSheet(cue);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->files.size() == 2);
    REQUIRE(parsed->files[0].name == "disc a.flac");
    REQUIRE(parsed->files[1].name == "disc b.flac");
    REQUIRE(parsed->files[1].tracks.front().number == 2);
    REQUIRE(parsed->files[1].tracks.front().indexes.front().minute == 0);
}

TEST_CASE("cue parser rejects overflow and structural limits", "[cue][parser][limit]")
{
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet("FILE \"a.flac\" FLAC\n" + LongText(tagreader_cue::kMaxCueLines, '\n')).has_value());

    std::string tooManyFiles;
    for (std::size_t index = 0; index < tagreader_cue::kMaxCueFileRefs + 1; ++index)
    {
        tooManyFiles += "FILE \"f" + std::to_string(index) + ".flac\" FLAC\n";
    }
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet(tooManyFiles).has_value());

    std::string tooManyTracks = "FILE \"f.flac\" FLAC\n";
    for (std::size_t index = 1; index <= tagreader_cue::kMaxCueTracks + 1; ++index)
    {
        tooManyTracks += "TRACK " + (index < 10 ? std::string("0") : std::string()) + std::to_string(index) + " AUDIO\n";
    }
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet(tooManyTracks).has_value());

    std::string tooManyIndexes = "FILE \"f.flac\" FLAC\nTRACK 01 AUDIO\n";
    for (std::size_t index = 1; index <= tagreader_cue::kMaxCueIndexesPerTrack + 1; ++index)
    {
        tooManyIndexes += "INDEX " + (index < 10 ? std::string("0") : std::string()) + std::to_string(index) + " 00:00:00\n";
    }
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet(tooManyIndexes).has_value());

    const std::string longField = "TITLE \"" + LongText(tagreader_cue::kMaxCueFieldBytes + 1, 'x') + "\"\n";
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet(longField).has_value());
}

TEST_CASE("cue parser rejects invalid index values", "[cue][parser][invalid]")
{
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet("FILE \"f.flac\" FLAC\nTRACK 01 AUDIO\nINDEX 01 00:00:75\n").has_value());
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet("FILE \"f.flac\" FLAC\nTRACK 01 AUDIO\nINDEX 01 00:60:00\n").has_value());
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet("FILE \"f.flac\" FLAC\nTRACK 01 AUDIO\nINDEX 01 999999999999:00:00\n").has_value());
    REQUIRE_FALSE(tagreader_cue::ParseCueSheet("FILE \"f.flac\" FLAC\nTRACK 01 AUDIO\nINDEX 01 70000:00:00\n").has_value());
}

TEST_CASE("cue read falls back to same-directory cover priority when audio lacks embedded cover", "[cue][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_fallback");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(WriteCoverImage(root / "folder.png", false));
    REQUIRE(WriteCoverImage(root / "cover.jpg", true));
    REQUIRE(WriteBytes(cuePath, Utf8Bytes("TITLE \"Album\"\nPERFORMER \"Artist\"\nFILE \"audio.mp3\" MP3\n  TRACK 01 AUDIO\n    TITLE \"Song\"\n    INDEX 01 00:00:00\n")));

    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(cuePath, root / "export");
    REQUIRE(tags.size() == 1);
    REQUIRE(!tags.front().coverPath().empty());
    INFO(tags.front().coverPath().string());
    const std::vector<std::uint8_t> expectedCoverBytes = tagreader_test_support::OneByOneJpeg();
    const std::vector<std::uint8_t> expectedFolderBytes = tagreader_test_support::OneByOnePng();
    const std::filesystem::path expectedCoverPath = tagreader_cover::WriteCoverAsPng(root / "export", expectedCoverBytes.data(), expectedCoverBytes.size());
    const std::filesystem::path expectedFolderPath = tagreader_cover::WriteCoverAsPng(root / "export", expectedFolderBytes.data(), expectedFolderBytes.size());
    INFO(expectedCoverPath.string());
    INFO(expectedFolderPath.string());
    REQUIRE(ExpectPathEqual(tags.front().coverPath(), expectedCoverPath));
}

TEST_CASE("cue read keeps embedded cover over same-directory fallback", "[cue][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_embedded_wins");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio-with-cover.mp3";
    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::GenerateCoverSample(audioPath));
    REQUIRE(WriteCoverImage(root / "cover.jpg", false));
    REQUIRE(WriteBytes(cuePath, Utf8Bytes("TITLE \"Album\"\nPERFORMER \"Artist\"\nFILE \"audio-with-cover.mp3\" MP3\n  TRACK 01 AUDIO\n    TITLE \"Song\"\n    INDEX 01 00:00:00\n")));

    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(cuePath, root / "export");
    REQUIRE(tags.size() == 1);
    REQUIRE(!tags.front().coverPath().empty());
    REQUIRE(std::filesystem::exists(tags.front().coverPath()));
    REQUIRE(tags.front().coverPath().filename() != "cover.png");
}
