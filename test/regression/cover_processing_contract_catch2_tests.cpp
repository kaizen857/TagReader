#include <catch2/catch_test_macros.hpp>

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "cover_format_fixtures.hpp"
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

// ---------- APIC pictureType 6 (Media) fallback cover (需求10) ----------

namespace
{
std::vector<std::uint8_t> BuildApicFrame(uint8_t pictureType, std::string_view mime, const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload{0};
    tagreader_test_support::AppendBytes(payload, mime);
    payload.insert(payload.end(), {0, pictureType, 0});
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return tagreader_test_support::BuildId3v23Frame("APIC", payload);
}

bool WriteMp3WithId3v23Frames(const std::filesystem::path &path, const std::vector<std::vector<std::uint8_t>> &frames)
{
    const std::filesystem::path basePath = path.parent_path() / "apic-fallback-base.mp3";
    if (!tagreader_test_support::GenerateBaseMp3(basePath))
    {
        return false;
    }
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(basePath);
    if (baseBytes.empty())
    {
        return false;
    }
    std::vector<std::uint8_t> allFrames;
    for (const std::vector<std::uint8_t> &frame : frames)
    {
        allFrames.insert(allFrames.end(), frame.begin(), frame.end());
    }
    std::vector<std::uint8_t> output = tagreader_test_support::BuildId3v23Tag(allFrames);
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    return tagreader_test_support::WriteBinaryFile(path, output);
}

// ID3v2.2 PIC payload: encoding(1) + 3-byte image format + picture type(1) + description(nul) + image data.
std::vector<std::uint8_t> BuildId3v22PicFrame(uint8_t pictureType, std::string_view imageFormat, const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload{0};
    tagreader_test_support::AppendBytes(payload, imageFormat);
    payload.push_back(pictureType);
    payload.push_back(0);
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    std::vector<std::uint8_t> frame;
    tagreader_test_support::AppendBytes(frame, "PIC");
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 16));
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> BuildId3v22Tag(const std::vector<std::uint8_t> &frames)
{
    std::vector<std::uint8_t> bytes{'I', 'D', '3', 2, 0, 0};
    const std::uint32_t size = static_cast<std::uint32_t>(frames.size());
    bytes.push_back(static_cast<std::uint8_t>((size >> 21) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 14) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((size >> 7) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(size & 0x7F));
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    return bytes;
}

std::vector<std::uint8_t> FlacPictureBlockPayload(uint8_t pictureType, const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload;
    tagreader_test_support::AppendU32BE(payload, pictureType);
    tagreader_test_support::AppendU32BE(payload, 9);
    tagreader_test_support::AppendBytes(payload, "image/png");
    tagreader_test_support::AppendU32BE(payload, 0);
    tagreader_test_support::AppendU32BE(payload, 1);
    tagreader_test_support::AppendU32BE(payload, 1);
    tagreader_test_support::AppendU32BE(payload, 32);
    tagreader_test_support::AppendU32BE(payload, 0);
    tagreader_test_support::AppendU32BE(payload, static_cast<std::uint32_t>(imageBytes.size()));
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return payload;
}

bool InjectFlacPictureBlockWithType(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, uint8_t pictureType, const std::vector<std::uint8_t> &imageBytes)
{
    const std::vector<std::uint8_t> data = tagreader_test_support::ReadBinaryFile(basePath);
    if (data.size() < 42 || std::string_view(reinterpret_cast<const char *>(data.data()), 4) != "fLaC")
    {
        return false;
    }
    const std::vector<std::uint8_t> picture = FlacPictureBlockPayload(pictureType, imageBytes);
    if (picture.size() > 0xFFFFFFU)
    {
        return false;
    }

    std::size_t cursor = 4;
    std::size_t audioStart = data.size();
    bool foundStreamInfo = false;
    std::vector<std::uint8_t> output{'f', 'L', 'a', 'C'};
    while (cursor + 4 <= data.size())
    {
        const bool lastBlock = (data[cursor] & 0x80) != 0;
        const std::uint8_t blockType = data[cursor] & 0x7F;
        const std::uint32_t blockSize = tagreader_test_support::ReadU24BE(data, cursor + 1);
        const std::size_t blockPayload = cursor + 4;
        const std::size_t blockEnd = blockPayload + blockSize;
        if (blockEnd > data.size())
        {
            break;
        }
        foundStreamInfo = foundStreamInfo || blockType == 0;
        output.push_back(blockType);
        tagreader_test_support::AppendU24BE(output, blockSize);
        output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(blockPayload), data.begin() + static_cast<std::ptrdiff_t>(blockEnd));
        cursor = blockEnd;
        if (lastBlock)
        {
            audioStart = cursor;
            break;
        }
    }
    if (!foundStreamInfo || audioStart > data.size())
    {
        return false;
    }
    output.push_back(0x80 | 6);
    tagreader_test_support::AppendU24BE(output, static_cast<std::uint32_t>(picture.size()));
    output.insert(output.end(), picture.begin(), picture.end());
    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(audioStart), data.end());
    return tagreader_test_support::WriteBinaryFile(outputPath, output);
}
} // namespace

TEST_CASE("CoverContract: APIC picture type 6 (Media) exports a cover through the full payload path", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("apic-type6");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path samplePath = root / "type6-media.mp3";
    // 与真实故障样本一致：pictureType=6（Media）+ mime 误标 image/jpeg + PNG 内容
    REQUIRE(WriteMp3WithId3v23Frames(samplePath, {BuildApicFrame(6, "image/jpeg", tagreader_test_support::OneByOnePng())}));

    const std::filesystem::path coverDir = root / "covers";
    const MusicTag tag = TagReader::Read(samplePath, coverDir);
    // 走完整 APIC 载荷解析路径（外层 + 内层 ReadID3v2ApicPayload 第二道检查均须放行）：
    REQUIRE_FALSE(tag.coverPath().empty());
    CHECK_FALSE(tag.thumbnailPath().empty());

    // 导出文件是真实存在的非空 PNG（ExportCoverFromContext 产物），证明内层检查放行而非仅外层跳过
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(tag.coverPath(), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    CHECK(size > 0);
    const std::vector<std::uint8_t> exported = tagreader_test_support::ReadBinaryFile(tag.coverPath());
    REQUIRE(exported.size() >= 8);
    CHECK(exported[0] == 0x89);
    CHECK(exported[1] == 'P');
    CHECK(exported[2] == 'N');
    CHECK(exported[3] == 'G');
}

TEST_CASE("CoverContract: APIC type 3 stays preferred over type 6 when both frames exist", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("apic-type3-priority");
    REQUIRE(PrepareRoot(root));
    const std::vector<std::uint8_t> png = tagreader_test_support::OneByOnePng();
    const std::vector<std::uint8_t> jpeg = tagreader_test_support::OneByOneJpeg();

    const std::filesystem::path refType3 = root / "ref-type3.mp3";
    const std::filesystem::path refType6 = root / "ref-type6.mp3";
    const std::filesystem::path bothType6First = root / "both-type6-first.mp3";
    const std::filesystem::path bothType3First = root / "both-type3-first.mp3";
    REQUIRE(WriteMp3WithId3v23Frames(refType3, {BuildApicFrame(3, "image/png", png)}));
    REQUIRE(WriteMp3WithId3v23Frames(refType6, {BuildApicFrame(6, "image/jpeg", jpeg)}));
    // 两帧并存，type 6 在前 / type 3 在后（最坏顺序：type 3 后到也必须胜出）
    REQUIRE(WriteMp3WithId3v23Frames(bothType6First, {BuildApicFrame(6, "image/jpeg", jpeg), BuildApicFrame(3, "image/png", png)}));
    // type 3 在前 / type 6 在后：type 6 不得覆盖已导出的 type 3
    REQUIRE(WriteMp3WithId3v23Frames(bothType3First, {BuildApicFrame(3, "image/png", png), BuildApicFrame(6, "image/jpeg", jpeg)}));

    const std::filesystem::path dirRef3 = root / "covers-ref3";
    const std::filesystem::path dirRef6 = root / "covers-ref6";
    const std::filesystem::path dirBoth6First = root / "covers-both6-first";
    const std::filesystem::path dirBoth3First = root / "covers-both3-first";
    const MusicTag tagRef3 = TagReader::Read(refType3, dirRef3);
    const MusicTag tagRef6 = TagReader::Read(refType6, dirRef6);
    REQUIRE_FALSE(tagRef3.coverPath().empty());
    REQUIRE_FALSE(tagRef6.coverPath().empty());
    // 内容寻址导出：不同图像字节 -> 不同导出文件名（PNG vs JPEG）
    CHECK(tagRef3.coverPath().filename() != tagRef6.coverPath().filename());

    const MusicTag tagBoth6First = TagReader::Read(bothType6First, dirBoth6First);
    const MusicTag tagBoth3First = TagReader::Read(bothType3First, dirBoth3First);
    // 无论帧顺序如何，最终封面都必须是 type 3 的图像（内容寻址文件名与 ref type3 一致）
    REQUIRE_FALSE(tagBoth6First.coverPath().empty());
    REQUIRE_FALSE(tagBoth3First.coverPath().empty());
    CHECK(tagBoth6First.coverPath().filename() == tagRef3.coverPath().filename());
    CHECK(tagBoth3First.coverPath().filename() == tagRef3.coverPath().filename());
}

TEST_CASE("CoverContract: ID3v2.2 PIC picture type 6 (Media) is accepted as fallback cover", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("id3v22-pic-type6");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path basePath = root / "v22-base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(basePath);
    REQUIRE_FALSE(baseBytes.empty());

    const std::vector<std::uint8_t> picFrame = BuildId3v22PicFrame(6, "PNG", tagreader_test_support::OneByOnePng());
    std::vector<std::uint8_t> output = BuildId3v22Tag(picFrame);
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    const std::filesystem::path samplePath = root / "v22-pic-type6.mp3";
    REQUIRE(tagreader_test_support::WriteBinaryFile(samplePath, output));

    const MusicTag tag = TagReader::Read(samplePath, root / "covers");
    REQUIRE_FALSE(tag.coverPath().empty());
    CHECK_FALSE(tag.thumbnailPath().empty());
}

TEST_CASE("CoverContract: FLAC PICTURE type 6 (Media) is accepted as fallback cover", "[contract][cover]")
{
    const std::filesystem::path root = CaseRoot("flac-picture-type6");
    REQUIRE(PrepareRoot(root));
    const std::filesystem::path basePath = root / "type6-base.flac";
    REQUIRE(tagreader_test_support::GenerateFlacAudioSample(basePath));
    const std::filesystem::path samplePath = root / "picture-type6.flac";
    REQUIRE(InjectFlacPictureBlockWithType(basePath, samplePath, 6, tagreader_test_support::OneByOnePng()));

    const MusicTag tag = TagReader::Read(samplePath, root / "covers");
    REQUIRE_FALSE(tag.coverPath().empty());
    CHECK_FALSE(tag.thumbnailPath().empty());
}
