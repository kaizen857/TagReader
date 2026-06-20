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

void WriteAudio(const std::filesystem::path &path)
{
    REQUIRE(tagreader_test_support::GenerateBaseMp3(path));
}

std::vector<MusicTag> ReadCue(const std::filesystem::path &cuePath, const std::string &cueText)
{
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, cueText));
    return TagReader::ReadCueSheet(cuePath);
}
}

TEST_CASE("cue timing converts frame indexes to microseconds", "[cue][time]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_time_convert");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "disc one.mp3";
    WriteAudio(audioPath);

    const std::string cue = "FILE \"disc one.mp3\" MP3\n"
                            "  TRACK 01 AUDIO\n"
                            "    INDEX 01 00:00:00\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 00:00:01\n";

    const std::vector<MusicTag> tags = ReadCue(root / "album.cue", cue);
    REQUIRE(tags.size() == 2);
    REQUIRE(tags[0].offset() == 0);
    REQUIRE(tags[1].offset() == 13333);
}

TEST_CASE("cue timing derives duration from next track and audio tail", "[cue][duration]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_duration");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "disc one.mp3";
    WriteAudio(audioPath);

    const std::string cue = "FILE \"disc one.mp3\" MP3\n"
                            "  TRACK 01 AUDIO\n"
                            "    INDEX 01 00:00:00\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 00:00:01\n";

    const std::vector<MusicTag> tags = ReadCue(root / "album.cue", cue);
    REQUIRE(tags.size() == 2);
    REQUIRE(tags[0].duration() == 13333);
    REQUIRE(tags[1].duration() >= 0);
}

TEST_CASE("cue timing handles multiple files independently", "[cue][time][multifile]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_time_multifile");
    REQUIRE(EnsureCleanRoot(root));

    WriteAudio(root / "a.mp3");
    WriteAudio(root / "b.mp3");

    const std::string cue = "FILE \"a.mp3\" MP3\n"
                            "  TRACK 01 AUDIO\n"
                            "    INDEX 01 00:00:00\n"
                            "FILE \"b.mp3\" MP3\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 00:00:00\n";

    const std::vector<MusicTag> tags = ReadCue(root / "album.cue", cue);
    REQUIRE(tags.size() == 2);
    REQUIRE(tags[0].filePath() == root / "a.mp3");
    REQUIRE(tags[1].filePath() == root / "b.mp3");
    REQUIRE(tags[0].offset() == 0);
    REQUIRE(tags[1].offset() == 0);
}

TEST_CASE("cue timing rejects invalid backward and overflow indexes", "[cue][time][invalid]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_time_invalid");
    REQUIRE(EnsureCleanRoot(root));

    WriteAudio(root / "disc.mp3");

    const std::string backwardCue = "FILE \"disc.mp3\" MP3\n"
                                    "  TRACK 01 AUDIO\n"
                                    "    INDEX 01 00:01:00\n"
                                    "  TRACK 02 AUDIO\n"
                                    "    INDEX 01 00:00:00\n";
    REQUIRE(tagreader_test_support::WriteTextFile(root / "backward.cue", backwardCue));
    REQUIRE(TagReader::ReadCueSheet(root / "backward.cue").empty());

    const std::string invalidFrameCue = "FILE \"disc.mp3\" MP3\n"
                                       "  TRACK 01 AUDIO\n"
                                       "    INDEX 01 00:00:75\n";
    REQUIRE(tagreader_test_support::WriteTextFile(root / "invalid-frame.cue", invalidFrameCue));
    REQUIRE(TagReader::ReadCueSheet(root / "invalid-frame.cue").empty());

    const std::string overflowCue = "FILE \"disc.mp3\" MP3\n"
                                    "  TRACK 01 AUDIO\n"
                                    "    INDEX 01 999999999999:00:00\n";
    REQUIRE(tagreader_test_support::WriteTextFile(root / "overflow.cue", overflowCue));
    REQUIRE(TagReader::ReadCueSheet(root / "overflow.cue").empty());
}
