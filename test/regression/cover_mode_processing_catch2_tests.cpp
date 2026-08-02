#include <catch2/catch_test_macros.hpp>

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "core/CoverErrorPolicy.hpp"
#include "core/ReadContext.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
constexpr std::string_view kTypedTitle = "typed-title";
constexpr std::string_view kTypedLyric = "typed-line-one";

std::size_t CountPngFiles(const std::filesystem::path &root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        return 0;
    }
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

std::size_t CountFiles(const std::filesystem::path &root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        return 0;
    }
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            ++count;
        }
    }
    return count;
}

bool ContainsTempFile(const std::filesystem::path &root)
{
    std::error_code ec;
    bool found = false;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it->path().filename().string().find(".tmp.") != std::string::npos)
        {
            found = true;
            break;
        }
    }
    return found;
}

std::filesystem::path CaseRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_cover_mode_processing_catch2" / std::string(caseName);
}

bool PrepareRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

// MP3 with TIT2 + USLT + APIC so metadata and lyrics are observable alongside art.
bool GenerateRichSample(const std::filesystem::path &samplePath)
{
    const std::filesystem::path basePath = samplePath.parent_path() / "base.mp3";
    if (!tagreader_test_support::GenerateBaseMp3(basePath))
    {
        return false;
    }
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(basePath);
    if (baseBytes.empty())
    {
        return false;
    }

    std::vector<std::uint8_t> titlePayload{0};
    tagreader_test_support::AppendBytes(titlePayload, kTypedTitle);

    std::vector<std::uint8_t> usltPayload{0, 'e', 'n', 'g', 0};
    tagreader_test_support::AppendBytes(usltPayload, kTypedLyric);

    std::vector<std::uint8_t> apicPayload{0};
    tagreader_test_support::AppendBytes(apicPayload, "image/png");
    apicPayload.insert(apicPayload.end(), {0, 3, 0});
    const std::vector<std::uint8_t> png = tagreader_test_support::OneByOnePng();
    apicPayload.insert(apicPayload.end(), png.begin(), png.end());

    std::vector<std::uint8_t> frames = tagreader_test_support::BuildId3v23Frame("TIT2", titlePayload);
    const std::vector<std::uint8_t> usltFrame = tagreader_test_support::BuildId3v23Frame("USLT", usltPayload);
    frames.insert(frames.end(), usltFrame.begin(), usltFrame.end());
    const std::vector<std::uint8_t> apicFrame = tagreader_test_support::BuildId3v23Frame("APIC", apicPayload);
    frames.insert(frames.end(), apicFrame.begin(), apicFrame.end());

    std::vector<std::uint8_t> output = tagreader_test_support::BuildId3v23Tag(frames);
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    return tagreader_test_support::WriteBinaryFile(samplePath, output);
}

void CheckMetadataAndLyrics(const MusicTag &tag)
{
    CHECK(tag.title() == kTypedTitle);
    REQUIRE_FALSE(tag.lyrics().empty());
    CHECK(tag.lyrics().lyrics().front().text() == kTypedLyric);
}
}

TEST_CASE("CoverMode: each processing mode exports exactly its output set", "[cover][mode]")
{
    const std::filesystem::path root = CaseRoot("mode-output-sets");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));

    const auto run = [&](CoverProcessingOptions::CoverProcessingMode mode,
                         bool expectFull, bool expectThumbnail, std::size_t expectedPngs,
                         std::string_view dirName)
    {
        CoverProcessingOptions options;
        options.mode = mode;
        const std::filesystem::path exportDir = root / dirName;
        const MusicTag tag = TagReader::Read(samplePath, exportDir, options);
        INFO(std::string(dirName));
        CHECK(tag.coverPath().empty() == !expectFull);
        CHECK(tag.thumbnailPath().empty() == !expectThumbnail);
        if (expectFull)
        {
            CHECK(std::filesystem::is_regular_file(tag.coverPath()));
        }
        if (expectThumbnail)
        {
            CHECK(std::filesystem::is_regular_file(tag.thumbnailPath()));
        }
        CHECK(CountPngFiles(exportDir) == expectedPngs);
    };

    run(CoverProcessingOptions::CoverProcessingMode::Disabled, false, false, 0, "disabled");
    run(CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly, false, true, 1, "thumb");
    run(CoverProcessingOptions::CoverProcessingMode::FullOnly, true, false, 1, "full");
    run(CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail, true, true, 2, "both");
}

TEST_CASE("CoverMode: Disabled ignores hostile cover directories with zero files", "[cover][mode][disabled]")
{
    const std::filesystem::path root = CaseRoot("disabled-hostile");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));

    const auto assertDisabledRead = [&](const std::filesystem::path &dir)
    {
        CoverProcessingOptions options;
        options.mode = CoverProcessingOptions::CoverProcessingMode::Disabled;
        const MusicTag tag = TagReader::Read(samplePath, dir, options);
        CHECK(tag.coverPath().empty());
        CHECK(tag.thumbnailPath().empty());
        CheckMetadataAndLyrics(tag);
    };

    {
        const std::filesystem::path unwritable = root / "unwritable";
        REQUIRE(std::filesystem::create_directory(unwritable));
        std::error_code ec;
        std::filesystem::permissions(unwritable, std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::remove, ec);
        REQUIRE(!ec);
        assertDisabledRead(unwritable);
        std::filesystem::permissions(unwritable, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        CHECK(CountFiles(unwritable) == 0);
    }
    {
        const std::filesystem::path target = root / "symlink-target";
        REQUIRE(std::filesystem::create_directory(target));
        const std::filesystem::path link = root / "link";
        REQUIRE(tagreader_test_support::PrepareSymlink(target, link));
        assertDisabledRead(link);
        CHECK(CountFiles(target) == 0);
    }
    {
        const std::filesystem::path parent = root / "sealed-parent";
        REQUIRE(std::filesystem::create_directory(parent));
        std::error_code ec;
        std::filesystem::permissions(parent, std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::remove, ec);
        REQUIRE(!ec);
        const std::filesystem::path missing = parent / "missing";
        assertDisabledRead(missing);
        std::filesystem::permissions(parent, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        CHECK_FALSE(std::filesystem::exists(missing));
    }
}

TEST_CASE("CoverMode: Disabled never resolves or creates the default cover directory", "[cover][mode][disabled]")
{
    const std::filesystem::path root = CaseRoot("disabled-default-dir");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    const std::filesystem::path runtimeRoot = root / "runtime";
    REQUIRE(std::filesystem::create_directory(runtimeRoot));
    REQUIRE(tagreader_test_support::SetEnvironment("XDG_RUNTIME_DIR", runtimeRoot));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::Disabled;
    const MusicTag tag = TagReader::Read(samplePath, "", options);
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
    CHECK_FALSE(std::filesystem::exists(runtimeRoot / "tagreader-covers"));
}

TEST_CASE("CoverMode: enabled Ignore preserves metadata and lyrics when cover directory setup fails", "[cover][mode][ignore]")
{
    const std::filesystem::path root = CaseRoot("ignore-dir-failure");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));
    const std::filesystem::path target = root / "target";
    REQUIRE(std::filesystem::create_directory(target));
    const std::filesystem::path link = root / "link";
    REQUIRE(tagreader_test_support::PrepareSymlink(target, link));

    CoverProcessingOptions options;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const MusicTag tag = TagReader::Read(samplePath, link, options);
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
    CheckMetadataAndLyrics(tag);
    CHECK(CountFiles(target) == 0);
}

TEST_CASE("CoverMode: ExportDirectoryUnavailable is thrown under Propagate with the directory path", "[cover][mode][typed]")
{
    const std::filesystem::path root = CaseRoot("export-dir-unavailable");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));
    const std::filesystem::path target = root / "target";
    REQUIRE(std::filesystem::create_directory(target));
    const std::filesystem::path link = root / "link";
    REQUIRE(tagreader_test_support::PrepareSymlink(target, link));

    try
    {
        (void)TagReader::Read(samplePath, link, CoverProcessingOptions{});
        FAIL("ExportDirectoryUnavailable should propagate with default policy");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::ExportDirectoryUnavailable);
        REQUIRE(ex.path().has_value());
        CHECK(ex.path()->string() == link.string());
        CHECK(std::string(ex.what()).find("cover export") != std::string::npos);
    }
    CHECK(CountFiles(target) == 0);
}

TEST_CASE("CoverMode: CacheWriteFailed is thrown or ignored by policy", "[cover][mode][typed]")
{
    const std::filesystem::path root = CaseRoot("cache-write-failed");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));
    const std::filesystem::path exportDir = root / "export";
    REQUIRE(std::filesystem::create_directory(exportDir));
    REQUIRE(tagreader_test_support::WriteBinaryFile(exportDir / "thumbnails",
                                                    tagreader_test_support::Bytes("not a directory")));

    try
    {
        (void)TagReader::Read(samplePath, exportDir, CoverProcessingOptions{});
        FAIL("CacheWriteFailed should propagate with default policy");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::CacheWriteFailed);
        CHECK(ex.path().has_value());
    }

    CoverProcessingOptions ignore;
    ignore.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const MusicTag tag = TagReader::Read(samplePath, exportDir, ignore);
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
    CheckMetadataAndLyrics(tag);
}

TEST_CASE("CoverMode: source budget boundaries size-1/exact/+1 and zero budget", "[cover][mode][budget]")
{
    const std::filesystem::path root = CaseRoot("budget-boundaries");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));
    const std::size_t pngSize = tagreader_test_support::OneByOnePng().size();

    auto readWithBudget = [&](std::uint64_t budget)
    {
        CoverProcessingOptions options;
        options.maxSourceCoverBytes = budget;
        return TagReader::Read(samplePath, root / ("export-" + std::to_string(budget)), options);
    };

    try
    {
        (void)readWithBudget(pngSize - 1);
        FAIL("one byte over budget should raise SourceBudgetExceeded");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::SourceBudgetExceeded);
    }

    const MusicTag atLimit = readWithBudget(pngSize);
    CHECK_FALSE(atLimit.coverPath().empty());
    CHECK_FALSE(atLimit.thumbnailPath().empty());
    CHECK(atLimit.title() == kTypedTitle);

    const MusicTag belowLimit = readWithBudget(pngSize + 1);
    CHECK_FALSE(belowLimit.coverPath().empty());
    CHECK_FALSE(belowLimit.thumbnailPath().empty());

    // Zero budget disables source-art reads without degrading metadata.
    CoverProcessingOptions zero;
    zero.maxSourceCoverBytes = 0;
    const MusicTag zeroBudget = TagReader::Read(samplePath, root / "export-zero", zero);
    CHECK(zeroBudget.coverPath().empty());
    CHECK(zeroBudget.thumbnailPath().empty());
    CHECK(zeroBudget.title() == kTypedTitle);
}

TEST_CASE("CoverMode: Ignore clears artwork when the source budget is exceeded", "[cover][mode][budget][ignore]")
{
    const std::filesystem::path root = CaseRoot("budget-ignore");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(GenerateRichSample(samplePath));
    const std::size_t pngSize = tagreader_test_support::OneByOnePng().size();

    CoverProcessingOptions options;
    options.maxSourceCoverBytes = pngSize - 1;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const MusicTag tag = TagReader::Read(samplePath, root / "export", options);
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
    CheckMetadataAndLyrics(tag);
}

TEST_CASE("CoverMode: cache hits reuse files without rewriting", "[cover][mode][cache]")
{
    const std::filesystem::path root = CaseRoot("cache-hit");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    const std::filesystem::path exportDir = root / "export";

    const MusicTag first = TagReader::Read(samplePath, exportDir);
    REQUIRE_FALSE(first.coverPath().empty());
    REQUIRE_FALSE(first.thumbnailPath().empty());
    const auto firstFullTime = std::filesystem::last_write_time(first.coverPath());
    const auto firstThumbTime = std::filesystem::last_write_time(first.thumbnailPath());

    const MusicTag second = TagReader::Read(samplePath, exportDir);
    CHECK(second.coverPath() == first.coverPath());
    CHECK(second.thumbnailPath() == first.thumbnailPath());
    CHECK(std::filesystem::last_write_time(second.coverPath()) == firstFullTime);
    CHECK(std::filesystem::last_write_time(second.thumbnailPath()) == firstThumbTime);
    CHECK(CountPngFiles(exportDir) == 2);
}

TEST_CASE("CoverMode: concurrent writers publish atomically without partial files", "[cover][mode][concurrency]")
{
    const std::filesystem::path root = CaseRoot("concurrent-writers");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    const std::filesystem::path exportDir = root / "export";

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (int j = 0; j < 3; ++j)
            {
                (void)TagReader::Read(samplePath, exportDir);
            } });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    CHECK(CountPngFiles(exportDir) == 2);
    CHECK_FALSE(ContainsTempFile(exportDir));
}

TEST_CASE("CoverMode: ThumbnailOnly with embedded art and a sidecar keeps the thumbnail output set", "[cover][mode][sidecar]")
{
    const std::filesystem::path root = CaseRoot("thumbnail-with-sidecar");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "sample.mp3";
    REQUIRE(tagreader_test_support::GenerateCoverSample(samplePath));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg",
                                                    tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;
    const std::filesystem::path exportDir = root / "export";
    const MusicTag tag = TagReader::Read(samplePath, exportDir, options);
    CHECK(tag.coverPath().empty());
    REQUIRE_FALSE(tag.thumbnailPath().empty());
    CHECK(std::filesystem::is_regular_file(tag.thumbnailPath()));
    CHECK(CountPngFiles(exportDir) == 1);
}

TEST_CASE("CoverMode: every CoverErrorCode is honored by Ignore and Propagate at the boundary", "[cover][mode][typed]")
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
        INFO("code=" << static_cast<int>(code));
        const std::filesystem::path path = "/art/cover.png";

        tagreader_core::ReadContext context;
        CoverProcessingOptions ignore;
        ignore.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
        context.coverOptions = &ignore;
        CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{code, "boom", path}, context)
              == tagreader_core::CoverErrorAction::Ignored);

        CoverProcessingOptions propagate;
        context.coverOptions = &propagate;
        CHECK(tagreader_core::ClassifyCoverFailure(CoverProcessingError{code, "boom", path}, context)
              == tagreader_core::CoverErrorAction::Propagated);
    }
}
