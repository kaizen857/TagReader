#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "core/ReadContext.hpp"
#include "cover/SidecarCover.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <filesystem>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#include <string>
#include <string_view>
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

namespace
{
constexpr std::string_view kSidecarStems[] = {"cover", "front", "folder", "album", "artwork"};
constexpr std::string_view kSidecarExts[] = {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif", ".tiff"};

std::string CanonicalSidecarCandidateFileName(std::size_t index)
{
    for (const std::string_view stem : kSidecarStems)
    {
        for (const std::string_view ext : kSidecarExts)
        {
            if (index == 0)
            {
                return std::string(stem) + std::string(ext);
            }
            --index;
        }
    }
    return {};
}

void WriteCandidateFiles(const std::filesystem::path &dir, std::size_t begin, std::size_t end)
{
    for (std::size_t i = begin; i < end; ++i)
    {
        REQUIRE(WriteSparseFile(dir / CanonicalSidecarCandidateFileName(i), 1, 0x00));
    }
}

bool WriteTaggedMp3(const std::filesystem::path &output, std::string_view title, const std::vector<std::uint8_t> &apicData)
{
    const std::filesystem::path base = output.parent_path() / "__base.mp3";
    if (!tagreader_test_support::GenerateBaseMp3(base))
    {
        return false;
    }
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(base);
    if (baseBytes.empty())
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(base, ec);

    std::vector<std::uint8_t> titlePayload{0};
    tagreader_test_support::AppendBytes(titlePayload, title);
    std::vector<std::uint8_t> frames = tagreader_test_support::BuildId3v23Frame("TIT2", titlePayload);
    if (!apicData.empty())
    {
        std::vector<std::uint8_t> apicPayload{0};
        tagreader_test_support::AppendBytes(apicPayload, "image/png");
        apicPayload.insert(apicPayload.end(), {0, 3, 0});
        apicPayload.insert(apicPayload.end(), apicData.begin(), apicData.end());
        const std::vector<std::uint8_t> apicFrame = tagreader_test_support::BuildId3v23Frame("APIC", apicPayload);
        frames.insert(frames.end(), apicFrame.begin(), apicFrame.end());
    }

    std::vector<std::uint8_t> outputBytes = tagreader_test_support::BuildId3v23Tag(frames);
    outputBytes.insert(outputBytes.end(), baseBytes.begin(), baseBytes.end());
    return tagreader_test_support::WriteBinaryFile(output, outputBytes);
}
}

TEST_CASE("Sidecar cover discovery enforces the configured entry limit", "[Sidecar][cover][limit]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_entry_limit");
    REQUIRE(EnsureCleanRoot(root));
    const std::filesystem::path audioPath = root / "audio.mp3";

    CoverProcessingOptions options;
    options.maxSidecarEntries = 11;
    tagreader_core::ReadContext context;
    context.filePath = audioPath;
    context.coverExportDir = root / "export";
    context.coverOptions = &options;

    // Exactly 11 candidates are accepted: each one is read, debited and
    // rejected as not-an-image, so the read degrades to no-art.
    WriteCandidateFiles(root, 0, 11);
    const tagreader_cover::CoverPaths accepted = tagreader_cover::ExportSidecarCover(context);
    CHECK(accepted.fullSizePath.empty());
    CHECK(accepted.thumbnailPath.empty());
    CHECK(context.coverSourceBytesDebited == 11);

    // The 12th candidate trips the limit during discovery, before any read.
    tagreader_core::ReadContext limited;
    limited.filePath = audioPath;
    limited.coverExportDir = root / "export-2";
    limited.coverOptions = &options;
    WriteCandidateFiles(root, 11, 12);
    try
    {
        (void)tagreader_cover::ExportSidecarCover(limited);
        FAIL("12 sidecar candidates must trip the entry limit");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::SidecarEntryLimitExceeded);
        REQUIRE(ex.path().has_value());
        CHECK(ex.path()->string() == root.string());
    }
    CHECK(limited.coverSourceBytesDebited == 0);
}

TEST_CASE("Sidecar cover discovery shares the cumulative source budget", "[Sidecar][cover][budget]")
{
    constexpr std::uint64_t kBudget = 64ULL * 1024ULL * 1024ULL;
    CoverProcessingOptions options;

    const auto probe = [&](const std::filesystem::path &root)
    {
        tagreader_core::ReadContext context;
        context.filePath = root / "audio.mp3";
        context.coverExportDir = root / "export";
        context.coverOptions = &options;
        return context;
    };

    // 64 MiB - 1 followed by 1 byte lands exactly on the budget: accepted.
    const std::filesystem::path rootA = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_budget_exact");
    REQUIRE(EnsureCleanRoot(rootA));
    REQUIRE(WriteSparseFile(rootA / "cover.png", kBudget - 1, 0x00));
    REQUIRE(WriteSparseFile(rootA / "front.jpg", 1, 0x00));
    tagreader_core::ReadContext exact = probe(rootA);
    const tagreader_cover::CoverPaths exactPaths = tagreader_cover::ExportSidecarCover(exact);
    CHECK(exactPaths.fullSizePath.empty());
    CHECK(exactPaths.thumbnailPath.empty());
    CHECK(exact.coverSourceBytesDebited == kBudget);

    // One byte over the shared budget trips SourceBudgetExceeded.
    const std::filesystem::path rootB = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_budget_over");
    REQUIRE(EnsureCleanRoot(rootB));
    REQUIRE(WriteSparseFile(rootB / "cover.png", kBudget - 1, 0x00));
    REQUIRE(WriteSparseFile(rootB / "front.jpg", 2, 0x00));
    tagreader_core::ReadContext over = probe(rootB);
    try
    {
        (void)tagreader_cover::ExportSidecarCover(over);
        FAIL("cumulative sidecar bytes over the budget must throw");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::SourceBudgetExceeded);
    }

    // A single candidate of exactly the budget size is accepted.
    const std::filesystem::path rootC = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_budget_single");
    REQUIRE(EnsureCleanRoot(rootC));
    REQUIRE(WriteSparseFile(rootC / "cover.png", kBudget, 0x00));
    tagreader_core::ReadContext single = probe(rootC);
    const tagreader_cover::CoverPaths singlePaths = tagreader_cover::ExportSidecarCover(single);
    CHECK(singlePaths.fullSizePath.empty());
    CHECK(single.coverSourceBytesDebited == kBudget);
}

TEST_CASE("Sidecar cover discovery fails on an unreadable directory", "[Sidecar][cover][discovery]")
{
#if defined(_WIN32)
    SKIP("POSIX directory permissions do not make a Windows directory unreadable");
#elif defined(__unix__) || defined(__APPLE__)
    if (::geteuid() == 0)
    {
        SKIP("POSIX directory permissions do not restrict root (CI 容器以 root 运行)");
    }
#endif
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_unreadable_dir");
    REQUIRE(EnsureCleanRoot(root));
    const std::filesystem::path music = root / "music";
    REQUIRE(EnsureCleanRoot(music));

    std::error_code permEc;
    std::filesystem::permissions(music, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, permEc);
    REQUIRE(!permEc);

    CoverProcessingOptions options;
    tagreader_core::ReadContext context;
    context.filePath = music / "audio.mp3";
    context.coverExportDir = root / "export";
    context.coverOptions = &options;
    try
    {
        (void)tagreader_cover::ExportSidecarCover(context);
        FAIL("an unreadable directory must fail sidecar discovery");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::SidecarDiscoveryFailed);
        REQUIRE(ex.path().has_value());
        CHECK(ex.path()->string() == music.string());
    }

    std::filesystem::permissions(music, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permEc);
    REQUIRE(!permEc);
}

TEST_CASE("Sidecar entry limit failure under Ignore keeps metadata", "[Sidecar][cover][limit][ignore]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_ignore_limit");
    REQUIRE(EnsureCleanRoot(root));
    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(WriteTaggedMp3(audioPath, "sidecar-ignore-title", {}));
    CoverProcessingOptions options;
    options.maxSidecarEntries = 11;
    WriteCandidateFiles(root, 0, 12);

    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const MusicTag tag = TagReader::Read(audioPath, root / "export", options);
    CHECK(tag.title() == "sidecar-ignore-title");
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
}

TEST_CASE("Embedded and sidecar cover share one source budget end to end", "[Sidecar][cover][budget]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_shared_budget");
    REQUIRE(EnsureCleanRoot(root));
    const std::filesystem::path audioPath = root / "audio.mp3";
    const std::vector<std::uint8_t> corruptApic(600, 0xAB);
    REQUIRE(WriteTaggedMp3(audioPath, "shared-budget-title", corruptApic));
    REQUIRE(WriteSparseFile(root / "cover.png", 500, 0x00));

    // Propagate: embedded 600 + sidecar 500 = 1100 > 1000 budget.
    CoverProcessingOptions over;
    over.maxSourceCoverBytes = 1000;
    try
    {
        (void)TagReader::Read(audioPath, root / "export-over", over);
        FAIL("embedded + sidecar bytes over the shared budget must throw");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::SourceBudgetExceeded);
    }

    // Exactly 1100 bytes: accepted; both candidates are corrupt, so no-art.
    CoverProcessingOptions exact;
    exact.maxSourceCoverBytes = 1100;
    const MusicTag tagExact = TagReader::Read(audioPath, root / "export-exact", exact);
    CHECK(tagExact.title() == "shared-budget-title");
    CHECK(tagExact.coverPath().empty());
    CHECK(tagExact.thumbnailPath().empty());

    // Ignore: the over-budget failure clears only the cover.
    CoverProcessingOptions ignore;
    ignore.maxSourceCoverBytes = 1000;
    ignore.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const MusicTag tagIgnored = TagReader::Read(audioPath, root / "export-ignore", ignore);
    CHECK(tagIgnored.title() == "shared-budget-title");
    CHECK(tagIgnored.coverPath().empty());
    CHECK(tagIgnored.thumbnailPath().empty());
}

TEST_CASE("Zero source budget disables sidecar reads but keeps metadata", "[Sidecar][cover][budget]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("sidecar_cover_zero_budget");
    REQUIRE(EnsureCleanRoot(root));
    const std::filesystem::path audioPath = root / "audio.mp3";
    REQUIRE(WriteTaggedMp3(audioPath, "zero-budget-sidecar", {}));
    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "cover.jpg", tagreader_test_support::OneByOnePng()));

    CoverProcessingOptions options;
    options.maxSourceCoverBytes = 0;
    const MusicTag tag = TagReader::Read(audioPath, root / "export", options);
    CHECK(tag.title() == "zero-budget-sidecar");
    CHECK(tag.coverPath().empty());
    CHECK(tag.thumbnailPath().empty());
}
