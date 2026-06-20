#include "catch2_regression_support.hpp"
#include "../../src/formats/cue/CueParser.hpp"
#include "../../src/formats/cue/CuePathResolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

TEST_CASE("cue file resolver accepts safe relative paths with spaces and quotes", "[cue][path]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_path_safe_relative");
    REQUIRE(tagreader_test_support::PrepareCleanDirectory(root));

    const std::filesystem::path cueDir = root / "album";
    REQUIRE(std::filesystem::create_directories(cueDir));
    const std::filesystem::path cuePath = cueDir / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, "FILE \"disc one.flac\" FLAC\n"));
    const std::filesystem::path audioPath = cueDir / "disc one.flac";
    REQUIRE(tagreader_test_support::WriteBinaryFile(audioPath, std::vector<std::uint8_t>{'a', 'u', 'd', 'i', 'o'}));

    const auto resolved = tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("disc one.flac"));
    REQUIRE(resolved.status == tagreader_cue::CuePathResolutionStatus::Resolved);
    REQUIRE(resolved.resolvedPath == audioPath);
}

TEST_CASE("cue file resolver preserves multi file association", "[cue][path]")
{
    const std::string cue = "FILE \"disc a.flac\" FLAC\n"
                            "  TRACK 01 AUDIO\n"
                            "    INDEX 01 00:00:00\n"
                            "FILE \"disc b.flac\" FLAC\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 00:10:00\n";
    const auto parsed = tagreader_cue::ParseCueSheet(cue);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->files.size() == 2);
    REQUIRE(parsed->files[0].tracks.size() == 1);
    REQUIRE(parsed->files[1].tracks.size() == 1);
}

TEST_CASE("cue file resolver rejects absolute and escape paths", "[cue][path][absolute][escape]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_path_escape");
    REQUIRE(tagreader_test_support::PrepareCleanDirectory(root));

    const std::filesystem::path cueDir = root / "album";
    REQUIRE(std::filesystem::create_directories(cueDir));
    const std::filesystem::path cuePath = cueDir / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, "FILE \"album.cue\" FLAC\n"));

    REQUIRE(tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("/tmp/audio.flac")).status == tagreader_cue::CuePathResolutionStatus::AbsolutePath);
    REQUIRE(tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("../audio.flac")).status == tagreader_cue::CuePathResolutionStatus::PathEscape);
}

TEST_CASE("cue file resolver rejects symlink directory and self reference", "[cue][path][symlink][directory]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_path_symlink");
    REQUIRE(tagreader_test_support::PrepareCleanDirectory(root));

    const std::filesystem::path cueDir = root / "album";
    REQUIRE(std::filesystem::create_directories(cueDir));
    const std::filesystem::path cuePath = cueDir / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, "FILE \"album.cue\" FLAC\n"));

    const std::filesystem::path audioDir = cueDir / "audio-dir";
    REQUIRE(std::filesystem::create_directory(audioDir));
    REQUIRE(tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("audio-dir")).status == tagreader_cue::CuePathResolutionStatus::Directory);

    const std::filesystem::path outsideDir = root / "outside";
    REQUIRE(std::filesystem::create_directories(outsideDir));
    const std::filesystem::path outsideAudio = outsideDir / "real.flac";
    REQUIRE(tagreader_test_support::WriteBinaryFile(outsideAudio, std::vector<std::uint8_t>{'a', 'u', 'd', 'i', 'o'}));
    REQUIRE(tagreader_test_support::PrepareSymlink(outsideAudio, cueDir / "linked.flac"));
    REQUIRE(tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("linked.flac")).status == tagreader_cue::CuePathResolutionStatus::Symlink);

    REQUIRE(tagreader_cue::ResolveCueFileReference(cuePath, cuePath.filename()).status == tagreader_cue::CuePathResolutionStatus::SelfReference);
}

TEST_CASE("cue file resolver reports missing files", "[cue][path][missing]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_path_missing");
    REQUIRE(tagreader_test_support::PrepareCleanDirectory(root));

    const std::filesystem::path cueDir = root / "album";
    REQUIRE(std::filesystem::create_directories(cueDir));
    const std::filesystem::path cuePath = cueDir / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, "FILE \"missing.flac\" FLAC\n"));

    const auto resolved = tagreader_cue::ResolveCueFileReference(cuePath, std::filesystem::path("missing.flac"));
    REQUIRE(resolved.status == tagreader_cue::CuePathResolutionStatus::Missing);
}
