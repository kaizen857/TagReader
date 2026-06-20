#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "TagReader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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

MusicTag ExpectSingleCueTag(const std::filesystem::path &root, const std::string &cueText)
{
    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, cueText));

    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(cuePath);
    REQUIRE(tags.size() == 1);
    return tags.front();
}
}

TEST_CASE("cue mapping applies track and album fields", "[cue][mapping]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_mapping_fields");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::GenerateBaseMp3(root / "disc one.mp3"));

    const std::string cue = "TITLE \"cue album\"\n"
                            "PERFORMER \"cue album artist\"\n"
                            "REM GENRE Jazz\n"
                            "REM YEAR 2024\n"
                            "REM DISCNUMBER 2\n"
                            "FILE \"disc one.mp3\" MP3\n"
                            "  TRACK 01 AUDIO\n"
                            "    TITLE \"cue track\"\n"
                            "    PERFORMER \"cue artist\"\n"
                            "    SONGWRITER \"cue composer\"\n"
                            "    INDEX 01 00:00:00\n";

    const MusicTag tag = ExpectSingleCueTag(root, cue);
    REQUIRE(tag.title() == "cue track");
    REQUIRE(tag.artist() == "cue artist");
    REQUIRE(tag.composer() == "cue composer");
    REQUIRE(tag.album() == "cue album");
    REQUIRE(tag.albumArtist() == "cue album artist");
    REQUIRE(tag.genre() == "Jazz");
    REQUIRE(tag.year() == 2024);
    REQUIRE(tag.discNumber() == 2);
    REQUIRE(tag.trackNumber() == 1);
}

TEST_CASE("cue mapping preserves audio fields when cue has no override", "[cue][audiofill]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_audio_fill");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "disc one.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));

    const MusicTag audioTag = TagReader::Read(audioPath);

    const std::string cue = "FILE \"disc one.mp3\" MP3\n"
                            "  TRACK 01 AUDIO\n"
                            "    INDEX 01 00:00:00\n";

    const MusicTag tag = ExpectSingleCueTag(root, cue);
    REQUIRE(tag.sampleRate() == audioTag.sampleRate());
    REQUIRE(tag.bitDepth() == audioTag.bitDepth());
    REQUIRE(tag.bitRate() == audioTag.bitRate());
    REQUIRE(tag.channels() == audioTag.channels());
    REQUIRE(tag.format() == audioTag.format());
    REQUIRE(tag.lastModified().time_since_epoch().count() != 0);
    REQUIRE(tag.lyrics().empty());
    REQUIRE(tag.coverPath() == audioTag.coverPath());
    REQUIRE(tag.playCount() == audioTag.playCount());
    REQUIRE(tag.rating() == audioTag.rating());
    REQUIRE(tag.filePath() == audioPath);
}

TEST_CASE("cue read path stays unchanged for ordinary audio", "[cue][read][nonregression]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_read_unchanged");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "plain.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));

    const std::vector<MusicTag> plainTags = TagReader::ReadCueSheet(audioPath);
    REQUIRE(plainTags.empty());

    const MusicTag audioTag = TagReader::Read(audioPath);
    REQUIRE(audioTag.filePath() == audioPath);
}
