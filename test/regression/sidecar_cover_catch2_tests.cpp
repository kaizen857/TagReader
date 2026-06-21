#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
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

bool WriteSparseFile(const std::filesystem::path &path, const std::uintmax_t size, const std::uint8_t byte)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    if (size == 0)
    {
        return true;
    }

    output.seekp(static_cast<std::streamoff>(size - 1), std::ios::beg);
    output.put(static_cast<char>(byte));
    return output.good();
}
}

TEST_CASE("Sidecar cover fallback discovers cover.jpg for ordinary mp3", "[Sidecar][cover]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_mp3");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path coverPath = root / "cover.jpg";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(coverPath, tagreader_test_support::OneByOnePng()));

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
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));

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
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(!tag.coverPath().empty());
    REQUIRE(std::filesystem::exists(tag.coverPath()));
}

TEST_CASE("Sidecar cover fallback skips malformed same-directory image", "[Sidecar][cover][malformed]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_malformed");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(WriteCueLikeText(root / "cover.jpg", "not an image"));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(tag.coverPath().empty());
}

TEST_CASE("Sidecar cover fallback skips oversized same-directory image", "[Sidecar][cover][oversized]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_oversized");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(WriteSparseFile(root / "cover.png", 64ULL * 1024ULL * 1024ULL + 1ULL, 0x00));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(tag.coverPath().empty());
}

TEST_CASE("Sidecar cover fallback skips symlinked same-directory image", "[Sidecar][cover][symlink]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_symlink");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path realDir = root / "real-target";
    const std::filesystem::path coverLink = root / "cover.jpg";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(EnsureCleanRoot(realDir));
    REQUIRE(tagreader_test_support::WriteBinaryFile(realDir / "payload.png", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::PrepareSymlink(realDir, coverLink));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(tag.coverPath().empty());
}

TEST_CASE("Sidecar cover fallback returns empty coverPath when no candidate exists", "[Sidecar][cover][nocandidate]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_no_candidate");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));

    const MusicTag tag = TagReader::Read(audioPath, root / "export");
    REQUIRE(tag.coverPath().empty());
}

TEST_CASE("Sidecar cover fallback rejects explicit symlink export directory", "[Sidecar][cover][invaliddir]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_invalid_dir");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path exportTarget = root / "export-target";
    const std::filesystem::path exportLink = root / "export-link";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(EnsureCleanRoot(exportTarget));
    REQUIRE(tagreader_test_support::PrepareSymlink(exportTarget, exportLink));

    bool rejected = false;
    try
    {
        (void)TagReader::Read(audioPath, exportLink);
    }
    catch (const std::exception &ex)
    {
        const std::string message = ex.what();
        rejected = message.find("symlink") != std::string::npos || message.find("symbolic link") != std::string::npos;
    }

    REQUIRE(rejected);
}
