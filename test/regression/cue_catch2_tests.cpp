#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
