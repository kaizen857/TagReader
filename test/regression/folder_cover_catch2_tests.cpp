#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

TEST_CASE("Folder cover thumbnailPath equals Read sidecar thumbnailPath for identical cover bytes", "[FolderCover][cover][consistency]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_consistency");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::filesystem::path coverPath = root / "cover.jpg";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(audioPath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(coverPath, tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag viaRead = TagReader::Read(audioPath, root / "export", options);
    const MusicTag viaFolder = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);

    REQUIRE(!viaRead.thumbnailPath().empty());
    REQUIRE(!viaFolder.thumbnailPath().empty());
    CHECK(viaFolder.thumbnailPath().string() == viaRead.thumbnailPath().string());
    REQUIRE(std::filesystem::is_regular_file(viaFolder.thumbnailPath()));
}

TEST_CASE("Folder cover export prefers cover.jpg over folder.jpg when both exist", "[FolderCover][cover][priority]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_priority");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "folder.jpg", tagreader_test_support::OneByOneJpeg()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag both = TagReader::ExportFolderCover(root.string(), (root / "export-both").string(), options);
    REQUIRE(!both.thumbnailPath().empty());

    std::error_code ec;
    REQUIRE(std::filesystem::remove(root / "folder.jpg", ec));
    REQUIRE(!ec);

    const MusicTag onlyCover = TagReader::ExportFolderCover(root.string(), (root / "export-only").string(), options);
    REQUIRE(!onlyCover.thumbnailPath().empty());

    CHECK(both.thumbnailPath().filename() == onlyCover.thumbnailPath().filename());
}

TEST_CASE("Folder cover export matches case-insensitive Cover.JPG candidate", "[FolderCover][cover][case]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_case");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "Cover.JPG", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    REQUIRE(!tag.thumbnailPath().empty());
    REQUIRE(std::filesystem::is_regular_file(tag.thumbnailPath()));
}

TEST_CASE("Folder cover export ignores non-matching mycover.jpg candidate", "[FolderCover][cover][nomatch]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_mycover");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "mycover.jpg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export rejects .tif candidate", "[FolderCover][cover][extension]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_tif");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.tif", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export skips hidden .cover.jpg candidate", "[FolderCover][cover][hidden]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_hidden");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / ".cover.jpg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export skips directory entry disguised as cover.jpg", "[FolderCover][cover][directory]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_directory");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(EnsureCleanRoot(root / "cover.jpg"));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty for nonexistent directory", "[FolderCover][cover][missing]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_missing");
    REQUIRE(EnsureCleanRoot(root));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    REQUIRE_NOTHROW((void)TagReader::ExportFolderCover((root / "not-there").string(), (root / "export").string(), options));
    const MusicTag tag = TagReader::ExportFolderCover((root / "not-there").string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty for empty directory", "[FolderCover][cover][empty]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_empty");
    REQUIRE(EnsureCleanRoot(root));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty when only non-cover files exist", "[FolderCover][cover][nocandidate]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_noncover");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::GenerateBaseMp3(root / "audio.mp3"));
    REQUIRE(tagreader_test_support::WriteTextFile(root / "readme.txt", "not a cover"));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty for zero-byte cover.jpg", "[FolderCover][cover][zerobyte]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_zero");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(WriteSparseFile(root / "cover.jpg", 0, 0x00));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty for oversized cover.png", "[FolderCover][cover][oversized]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_oversized");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(WriteSparseFile(root / "cover.png", 64ULL * 1024ULL * 1024ULL + 1ULL, 0x00));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export returns empty for symlinked cover.jpg", "[FolderCover][cover][symlink]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_symlink");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path realDir = root / "real-target";
    REQUIRE(EnsureCleanRoot(realDir));
    REQUIRE(tagreader_test_support::WriteBinaryFile(realDir / "payload.png", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::PrepareSymlink(realDir, root / "cover.jpg"));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export swallows SidecarEntryLimitExceeded under low maxSidecarEntries", "[FolderCover][cover][limit]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_entry_limit");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.png", tagreader_test_support::OneByOnePng()));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "Cover.jpeg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;
    options.maxSidecarEntries = 2;

    REQUIRE_NOTHROW((void)TagReader::ExportFolderCover(root.string(), (root / "export").string(), options));
    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Folder cover export reuses the same cache path for identical bytes", "[FolderCover][cover][cache]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_cache_reuse");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag first = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    const MusicTag second = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);

    REQUIRE(!first.thumbnailPath().empty());
    REQUIRE(!second.thumbnailPath().empty());
    CHECK(first.thumbnailPath().string() == second.thumbnailPath().string());
    REQUIRE(std::filesystem::is_regular_file(first.thumbnailPath()));
}

TEST_CASE("Folder cover export in ThumbnailOnly mode writes no full-size artwork", "[FolderCover][cover][thumbnailonly]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("folder_cover_thumbnail_only");
    REQUIRE(EnsureCleanRoot(root));

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;

    const MusicTag tag = TagReader::ExportFolderCover(root.string(), (root / "export").string(), options);
    REQUIRE(!tag.thumbnailPath().empty());
    REQUIRE(std::filesystem::is_regular_file(tag.thumbnailPath()));
    CHECK(tag.coverPath().empty());
    CHECK(!std::filesystem::exists(root / "export" / "artwork"));
}
