#include <catch2/catch_test_macros.hpp>

#include "default_cover_export_directory_support.hpp"
#include "catch2_sample_support.hpp"

TEST_CASE("DefaultCover: fallback directory ignores legacy root symlink", "[regression][DefaultCover]")
{
#if defined(_WIN32)
    SKIP("default fallback directory fixture relies on POSIX semantics");
#endif
    const std::filesystem::path root = default_cover_test::MakeCaseRoot("fallback");
    REQUIRE(default_cover_test::PrepareCaseRoot(root));
    const std::filesystem::path samplePath = root / "sample" / "cover-policy.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(default_cover_test::RunDefaultFallbackIgnoresLegacyRootSymlink(root, samplePath));
}

TEST_CASE("DefaultCover: XDG runtime directory is preferred", "[regression][DefaultCover]")
{
#if defined(_WIN32)
    SKIP("XDG runtime directory is POSIX-specific");
#endif
    const std::filesystem::path root = default_cover_test::MakeCaseRoot("xdg-runtime");
    REQUIRE(default_cover_test::PrepareCaseRoot(root));
    const std::filesystem::path samplePath = root / "sample" / "cover-policy.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(default_cover_test::RunXdgRuntimeDefaultIsPreferred(root, samplePath));
}

TEST_CASE("DefaultCover: default root symlink is rejected", "[regression][DefaultCover]")
{
#if defined(_WIN32)
    SKIP("default root symlink fixture relies on POSIX semantics");
#endif
    const std::filesystem::path root = default_cover_test::MakeCaseRoot("runtime-symlink");
    REQUIRE(default_cover_test::PrepareCaseRoot(root));
    const std::filesystem::path samplePath = root / "sample" / "cover-policy.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(default_cover_test::RunDefaultRootSymlinkIsRejected(root, samplePath));
}

TEST_CASE("DefaultCover: explicit symlink directory is rejected", "[regression][DefaultCover]")
{
    const std::filesystem::path root = default_cover_test::MakeCaseRoot("explicit-symlink");
    REQUIRE(default_cover_test::PrepareCaseRoot(root));
    const std::filesystem::path samplePath = root / "sample" / "cover-policy.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(default_cover_test::RunExplicitSymlinkDirectoryIsRejected(root, samplePath));
}
