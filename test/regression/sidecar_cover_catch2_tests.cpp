#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

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

bool WriteCueLikeText(const std::filesystem::path &path, const std::string &text)
{
    return tagreader_test_support::WriteTextFile(path, text);
}
}

TEST_CASE("Sidecar cover fallback discovers cover.jpg for ordinary mp3", "[Sidecar][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_mp3");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path coverPath = root / "cover.jpg";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(coverPath, tagreader_test_support::OneByOneJpeg()));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(!tag.coverPath().empty());
    REQUIRE(std::filesystem::is_regular_file(tag.coverPath()));
}

TEST_CASE("Sidecar cover fallback keeps embedded cover over same-directory image", "[Sidecar][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_embedded_wins");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio-with-cover.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(audioPath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOneJpeg()));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(!tag.coverPath().empty());
    REQUIRE(std::filesystem::exists(tag.coverPath()));
}

TEST_CASE("Sidecar cover fallback prefers cover over folder when both exist", "[Sidecar][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_priority");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "folder.png", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOneJpeg()));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(!tag.coverPath().empty());
    REQUIRE(std::filesystem::exists(tag.coverPath()));
}
