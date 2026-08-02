#include <catch2/catch_test_macros.hpp>

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "core/CoverBudget.hpp"
#include "core/CoverErrorPolicy.hpp"
#include "core/RawTagData.hpp"
#include "core/ReadContext.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
constexpr std::uint64_t kDefaultSourceBudget = 64ULL * 1024 * 1024;

CoverProcessingOptions OptionsWith(CoverProcessingOptions::CoverFailurePolicy policy)
{
    CoverProcessingOptions options;
    options.failurePolicy = policy;
    return options;
}

std::size_t CountPngFiles(const std::filesystem::path &root)
{
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec) && it->path().extension() == ".png")
        {
            ++count;
        }
    }
    return count;
}

std::filesystem::path CaseRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_cover_processing_contract_catch2" / std::string(caseName);
}

bool PrepareRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

// Seeds the content-addressed full-size cache with a real PNG (FullOnly read),
// then replaces the cached file with a broken symlink. A broken symlink is the
// deterministic cache-path anomaly that reaches IsReusableCoverCacheFile's
// validation: exists() follows the link and reports not-found, while
// symlink_status() succeeds with type=symlink. (Pointing the cache path at a
// directory would instead short-circuit as a cache hit, so the symlink is the
// reliable file-type anomaly for this contract.) Returns false on any setup
// failure so the caller can REQUIRE it.
bool SeedBrokenSymlinkAtCachedCover(const std::filesystem::path &root, const std::filesystem::path &samplePath)
{
    const std::filesystem::path coverDir = root / "covers";
    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::FullOnly;
    const MusicTag first = TagReader::Read(samplePath, coverDir, options);
    if (first.coverPath().empty())
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(first.coverPath(), ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::create_symlink(root / "missing-target.png", first.coverPath(), ec);
    return !ec;
}

TEST_CASE("CoverContract: cached cover path anomaly is typed CacheReadFailed under Propagate and no-art under Ignore", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("cache-path-symlink");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(SeedBrokenSymlinkAtCachedCover(root, samplePath));

    const std::filesystem::path coverDir = root / "covers";
    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::FullOnly;

    SECTION("Propagate surfaces CacheReadFailed")
    {
        try
        {
            (void)TagReader::Read(samplePath, coverDir, options);
            FAIL("cache-path anomaly must propagate under the default policy");
        }
        catch (const CoverProcessingError &ex)
        {
            CHECK(ex.code() == CoverErrorCode::CacheReadFailed);
            const std::string message = ex.what();
            CHECK(message.find("cover cache") != std::string::npos);
            CHECK(message.find("symlink") != std::string::npos);
        }
        catch (const std::exception &ex)
        {
            FAIL("cache-path anomaly must surface as CoverProcessingError, got: " << ex.what());
        }
    }

    SECTION("Ignore swallows the failure as no-art")
    {
        options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
        const MusicTag tag = TagReader::Read(samplePath, coverDir, options);
        CHECK(tag.coverPath().empty());
    }
}
}

TEST_CASE("CoverContract: default options preserve current full+thumbnail propagate behavior", "[contract][cover]")
{
    const CoverProcessingOptions options{};
    CHECK(options.mode == CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail);
    CHECK(options.failurePolicy == CoverProcessingOptions::CoverFailurePolicy::Propagate);
    CHECK(options.generateThumbnail == true);
    CHECK(options.maxSidecarEntries == 4096U);
    CHECK(options.maxSourceCoverBytes == kDefaultSourceBudget);
    // Historical fields remain intact and unchanged.
    CHECK(options.thumbnailSize.width == 256U);
    CHECK(options.thumbnailSize.height == 256U);
    CHECK(options.thumbnailSize.maintainAspectRatio == true);
    CHECK(options.scalingQuality == CoverProcessingOptions::ScalingQuality::Fast);
    CHECK(options.pngCompression == CoverProcessingOptions::PngCompressionLevel::Fast);
}

TEST_CASE("CoverContract: explicit default options match the no-options read output set", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("default-equivalence");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));

    const std::filesystem::path dirA = root / "covers-a";
    const std::filesystem::path dirB = root / "covers-b";
    const MusicTag tagA = TagReader::Read(samplePath, dirA);
    const MusicTag tagB = TagReader::Read(samplePath, dirB, CoverProcessingOptions{});

    REQUIRE_FALSE(tagA.coverPath().empty());
    CHECK_FALSE(tagA.thumbnailPath().empty());
    // Both reads export the same content-addressed full and thumbnail PNGs.
    CHECK(tagA.coverPath().filename() == tagB.coverPath().filename());
    CHECK(tagA.thumbnailPath().filename() == tagB.thumbnailPath().filename());
    CHECK(CountPngFiles(dirA) == 2U);
    CHECK(CountPngFiles(dirB) == 2U);
}

TEST_CASE("CoverContract: each mode declares its output set", "[contract][cover]")
{
    struct OutputSet
    {
        bool full;
        bool thumbnail;
    };
    const auto outputs = [](CoverProcessingOptions::CoverProcessingMode mode)
    {
        switch (mode)
        {
        case CoverProcessingOptions::CoverProcessingMode::Disabled:
            return OutputSet{false, false};
        case CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly:
            return OutputSet{false, true};
        case CoverProcessingOptions::CoverProcessingMode::FullOnly:
            return OutputSet{true, false};
        case CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail:
            return OutputSet{true, true};
        }
        return OutputSet{false, false};
    };

    CHECK_FALSE(outputs(CoverProcessingOptions::CoverProcessingMode::Disabled).full);
    CHECK_FALSE(outputs(CoverProcessingOptions::CoverProcessingMode::Disabled).thumbnail);
    CHECK_FALSE(outputs(CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly).full);
    CHECK(outputs(CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly).thumbnail);
    CHECK(outputs(CoverProcessingOptions::CoverProcessingMode::FullOnly).full);
    CHECK_FALSE(outputs(CoverProcessingOptions::CoverProcessingMode::FullOnly).thumbnail);
    CHECK(outputs(CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail).full);
    CHECK(outputs(CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail).thumbnail);
}

TEST_CASE("CoverContract: every CoverErrorCode is constructible and what() is non-empty", "[contract][cover]")
{
    const std::vector<CoverErrorCode> codes{
        CoverErrorCode::ExportDirectoryUnavailable,
        CoverErrorCode::SidecarDiscoveryFailed,
        CoverErrorCode::SidecarEntryLimitExceeded,
        CoverErrorCode::SourceReadFailed,
        CoverErrorCode::SourceBudgetExceeded,
        CoverErrorCode::DecodeFailed,
        CoverErrorCode::CacheReadFailed,
        CoverErrorCode::CacheWriteFailed,
        CoverErrorCode::PublicationFailed,
    };
    for (const CoverErrorCode code : codes)
    {
        INFO(static_cast<int>(code));
        const CoverProcessingError error{code, "boom"};
        CHECK(error.code() == code);
        CHECK_FALSE(error.path().has_value());
        CHECK_FALSE(std::string(error.what()).empty());
        CHECK(std::string(error.what()).find("boom") != std::string::npos);
        CHECK(dynamic_cast<const std::runtime_error *>(&error) != nullptr);
    }
}

TEST_CASE("CoverContract: CoverProcessingError carries an optional path reported by what()", "[contract][cover]")
{
    const std::filesystem::path path = "/tmp/art/cover.png";
    const CoverProcessingError error{CoverErrorCode::DecodeFailed, "decode failed", path};
    CHECK(error.code() == CoverErrorCode::DecodeFailed);
    REQUIRE(error.path().has_value());
    CHECK(error.path()->string() == path.string());
    const std::string message = error.what();
    CHECK_FALSE(message.empty());
    CHECK(message.find("DecodeFailed") != std::string::npos);
    CHECK(message.find(path.string()) != std::string::npos);

    const CoverProcessingError withoutPath{CoverErrorCode::DecodeFailed, "decode failed"};
    CHECK_FALSE(withoutPath.path().has_value());
    CHECK(std::string(withoutPath.what()).find("(path:") == std::string::npos);
}

TEST_CASE("CoverContract: Ignore swallows only CoverProcessingError", "[contract][cover]")
{
    tagreader_core::ReadContext context;
    const CoverProcessingOptions options = OptionsWith(CoverProcessingOptions::CoverFailurePolicy::Ignore);
    context.coverOptions = &options;

    CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{CoverErrorCode::DecodeFailed, "decode"}, context)
          == tagreader_core::CoverErrorAction::Ignored);
    CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{CoverErrorCode::PublicationFailed, "pub", "/x.png"}, context)
          == tagreader_core::CoverErrorAction::Ignored);

    // Non-cover exceptions are never consumed or reclassified, even under Ignore.
    CHECK(tagreader_core::ClassifyCoverFailure(std::runtime_error{"media open failed"}, context)
          == tagreader_core::CoverErrorAction::NotACoverError);
    CHECK(tagreader_core::ClassifyCoverFailure(std::filesystem::filesystem_error{"stat", "/x", std::error_code{}}, context)
          == tagreader_core::CoverErrorAction::NotACoverError);
}

TEST_CASE("CoverContract: Propagate (default) keeps cover failures and ignores ordinary errors", "[contract][cover]")
{
    tagreader_core::ReadContext context;
    const CoverProcessingOptions options{}; // Propagate by default
    context.coverOptions = &options;

    CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{CoverErrorCode::CacheWriteFailed, "write"}, context)
          == tagreader_core::CoverErrorAction::Propagated);
    CHECK(tagreader_core::ClassifyCoverFailure(std::runtime_error{"ordinary metadata error"}, context)
          == tagreader_core::CoverErrorAction::NotACoverError);

    // Unconfigured contexts (no options injected) keep propagating cover failures.
    tagreader_core::ReadContext unconfigured;
    CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{CoverErrorCode::SourceReadFailed, "read"}, unconfigured)
          == tagreader_core::CoverErrorAction::Propagated);
}

TEST_CASE("CoverContract: Propagate boundary rethrows the identical code and path", "[contract][cover]")
{
    tagreader_core::ReadContext context;
    const CoverProcessingOptions options{}; // Propagate
    context.coverOptions = &options;
    const std::filesystem::path expectedPath = "/data/art/cover.png";

    try
    {
        try
        {
            throw CoverProcessingError{CoverErrorCode::PublicationFailed, "publish failed", expectedPath};
        }
        catch (const CoverProcessingError &ex)
        {
            REQUIRE(tagreader_core::ClassifyCoverFailure(ex, context) == tagreader_core::CoverErrorAction::Propagated);
            throw;
        }
        FAIL("CoverProcessingError should have propagated unchanged");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::PublicationFailed);
        REQUIRE(ex.path().has_value());
        CHECK(ex.path()->string() == expectedPath.string());
    }
    catch (const std::exception &ex)
    {
        FAIL("boundary must not reclassify a cover failure into another type: " << ex.what());
    }
}

TEST_CASE("CoverContract: Ignore boundary swallows the failure and clears artwork", "[contract][cover]")
{
    tagreader_core::ReadContext context;
    const CoverProcessingOptions options = OptionsWith(CoverProcessingOptions::CoverFailurePolicy::Ignore);
    context.coverOptions = &options;

    tagreader_core::RawMetadata metadata;
    metadata.coverPath = "/tmp/partial.png";
    metadata.thumbnailPath = "/tmp/partial-thumb.png";

    try
    {
        throw CoverProcessingError{CoverErrorCode::CacheWriteFailed, "cache write failed", "/tmp/art.png"};
    }
    catch (const CoverProcessingError &ex)
    {
        if (tagreader_core::ClassifyCoverFailure(ex, context) == tagreader_core::CoverErrorAction::Ignored)
        {
            metadata.coverPath.clear();
            metadata.thumbnailPath.clear();
        }
        else
        {
            throw;
        }
    }
    catch (const std::exception &)
    {
        FAIL("Ignore policy must never propagate a CoverProcessingError");
    }

    CHECK(metadata.coverPath.empty());
    CHECK(metadata.thumbnailPath.empty());
}

TEST_CASE("CoverContract: source budget boundaries at 64 MiB-1, exact and +1", "[contract][cover]")
{
    // A single candidate at or below the limit fits; one byte more exceeds it.
    CHECK_FALSE(tagreader_core::ExceedsCoverSourceBudget(0, kDefaultSourceBudget - 1, kDefaultSourceBudget));
    CHECK_FALSE(tagreader_core::ExceedsCoverSourceBudget(0, kDefaultSourceBudget, kDefaultSourceBudget));
    CHECK(tagreader_core::ExceedsCoverSourceBudget(0, kDefaultSourceBudget + 1, kDefaultSourceBudget));

    // Embedded bytes and sidecar fallback share the same cumulative budget.
    CHECK_FALSE(tagreader_core::ExceedsCoverSourceBudget(kDefaultSourceBudget - 1, 1, kDefaultSourceBudget));
    CHECK(tagreader_core::ExceedsCoverSourceBudget(kDefaultSourceBudget - 1, 2, kDefaultSourceBudget));
    CHECK(tagreader_core::ExceedsCoverSourceBudget(kDefaultSourceBudget, 1, kDefaultSourceBudget));
    CHECK_FALSE(tagreader_core::ExceedsCoverSourceBudget(kDefaultSourceBudget, 0, kDefaultSourceBudget));

    // A zero budget disables source-art reads; zero bytes still fit.
    CHECK(tagreader_core::ExceedsCoverSourceBudget(0, 1, 0));
    CHECK_FALSE(tagreader_core::ExceedsCoverSourceBudget(0, 0, 0));

    // Overflow-safe on absurd accumulated counters.
    CHECK(tagreader_core::ExceedsCoverSourceBudget(std::numeric_limits<std::uint64_t>::max(), 0, kDefaultSourceBudget));
}

TEST_CASE("CoverContract: zero source budget does not degrade metadata", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("zero-budget");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(basePath);
    REQUIRE_FALSE(baseBytes.empty());

    std::vector<std::uint8_t> apicPayload{0};
    tagreader_test_support::AppendBytes(apicPayload, "image/png");
    apicPayload.insert(apicPayload.end(), {0, 3, 0});
    const std::vector<std::uint8_t> png = tagreader_test_support::OneByOnePng();
    apicPayload.insert(apicPayload.end(), png.begin(), png.end());

    std::vector<std::uint8_t> titlePayload{0};
    tagreader_test_support::AppendBytes(titlePayload, "zero-budget-title");
    std::vector<std::uint8_t> frames = tagreader_test_support::BuildId3v23Frame("TIT2", titlePayload);
    const std::vector<std::uint8_t> apicFrame = tagreader_test_support::BuildId3v23Frame("APIC", apicPayload);
    frames.insert(frames.end(), apicFrame.begin(), apicFrame.end());

    std::vector<std::uint8_t> output = tagreader_test_support::BuildId3v23Tag(frames);
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    const std::filesystem::path samplePath = root / "zero-budget.mp3";
    REQUIRE(tagreader_test_support::WriteBinaryFile(samplePath, output));

    CoverProcessingOptions options;
    options.maxSourceCoverBytes = 0;
    const MusicTag tag = TagReader::Read(samplePath, root / "covers", options);
    CHECK(tag.title() == "zero-budget-title");
    CHECK_FALSE(tag.duration() == 0);
}
