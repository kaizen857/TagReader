#include <catch2/catch_test_macros.hpp>

#include "flac_malformed_metadata_support.hpp"

TEST_CASE("FlacMalformed: later valid Vorbis block survives", "[regression][FlacMalformed]")
{
    const std::filesystem::path root = flac_malformed_test::MakeCaseRoot("later-valid");
    REQUIRE(flac_malformed_test::PrepareCaseRoot(root));
    REQUIRE(flac_malformed_test::RunLaterValidVorbisBlockSurvives(root));
}

TEST_CASE("FlacMalformed: picture cover cache failure propagates", "[regression][FlacMalformed]")
{
    const std::filesystem::path root = flac_malformed_test::MakeCaseRoot("picture-error");
    REQUIRE(flac_malformed_test::PrepareCaseRoot(root));
    REQUIRE(flac_malformed_test::RunFlacPictureCoverCacheFailurePropagates(root));
}
