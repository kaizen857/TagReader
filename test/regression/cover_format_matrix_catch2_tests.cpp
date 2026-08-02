#include <catch2/catch_test_macros.hpp>

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "cover_format_fixtures.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::filesystem::path CaseRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_cover_format_matrix_catch2" / std::string(caseName);
}

bool PrepareRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

bool IsUnderThumbnails(const std::filesystem::path &root, const std::filesystem::path &file)
{
    const std::filesystem::path relative = file.lexically_relative(root);
    return !relative.empty() && *relative.begin() == "thumbnails";
}

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

std::size_t CountFullPngs(const std::filesystem::path &root)
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
        if (it->is_regular_file(ec) && it->path().extension() == ".png" && !IsUnderThumbnails(root, it->path()))
        {
            ++count;
        }
    }
    return count;
}

struct FormatCase
{
    std::string_view name;
    std::string_view fileName;
    bool (*generate)(const std::filesystem::path &path);
};

const FormatCase kFormats[] = {
    {"flac", "sample.flac", tagreader_test_support::GenerateFlacCoverSample},
    {"ogg-vorbis", "sample.ogg", tagreader_test_support::GenerateOggVorbisCoverSample},
    {"opus", "sample.opus", tagreader_test_support::GenerateOpusCoverSample},
    {"mp4", "sample.m4a", tagreader_test_support::GenerateMp4CoverSample},
    {"ape", "sample.mp3", tagreader_test_support::GenerateApeCoverSample},
    {"wav-id3", "sample.wav", tagreader_test_support::GenerateWavId3CoverSample},
    {"asf", "sample.wma", tagreader_test_support::GenerateAsfCoverSample},
    {"matroska", "sample.mka", tagreader_test_support::GenerateMatroskaCoverSample},
};

bool GenerateSample(const FormatCase &format, const std::filesystem::path &root)
{
    return format.generate(root / format.fileName);
}
} // namespace

TEST_CASE("CoverFormatMatrix: ThumbnailOnly exports zero full-size PNGs for every embedded-cover format", "[cover][mode][matrix][thumbnail-only]")
{
    for (const FormatCase &format : kFormats)
    {
        INFO("format=" << format.name);
        const std::filesystem::path root = CaseRoot(std::string("thumb-") + std::string(format.name));
        REQUIRE(PrepareRoot(root));
        REQUIRE(GenerateSample(format, root));

        CoverProcessingOptions options;
        options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;
        const std::filesystem::path exportDir = root / "export";
        const MusicTag tag = TagReader::Read(root / format.fileName, exportDir, options);

        CHECK(tag.coverPath().empty());
        REQUIRE_FALSE(tag.thumbnailPath().empty());
        CHECK(std::filesystem::is_regular_file(tag.thumbnailPath()));
        CHECK(tag.thumbnailPath().string().find("thumbnails") != std::string::npos);
        CHECK(CountPngFiles(exportDir) == 1);
        CHECK(CountFullPngs(exportDir) == 0);
    }
}

TEST_CASE("CoverFormatMatrix: FullOnly and FullAndThumbnail output sets match across formats", "[cover][mode][matrix]")
{
    for (const FormatCase &format : kFormats)
    {
        INFO("format=" << format.name);
        const std::filesystem::path root = CaseRoot(std::string("full-") + std::string(format.name));
        REQUIRE(PrepareRoot(root));
        REQUIRE(GenerateSample(format, root));

        {
            CoverProcessingOptions options;
            options.mode = CoverProcessingOptions::CoverProcessingMode::FullOnly;
            const std::filesystem::path exportDir = root / "export-full";
            const MusicTag tag = TagReader::Read(root / format.fileName, exportDir, options);
            REQUIRE_FALSE(tag.coverPath().empty());
            CHECK(std::filesystem::is_regular_file(tag.coverPath()));
            CHECK(tag.thumbnailPath().empty());
            CHECK(CountPngFiles(exportDir) == 1);
            CHECK(CountFullPngs(exportDir) == 1);
        }

        {
            CoverProcessingOptions options;
            options.mode = CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail;
            const std::filesystem::path exportDir = root / "export-both";
            const MusicTag tag = TagReader::Read(root / format.fileName, exportDir, options);
            REQUIRE_FALSE(tag.coverPath().empty());
            REQUIRE_FALSE(tag.thumbnailPath().empty());
            CHECK(std::filesystem::is_regular_file(tag.coverPath()));
            CHECK(std::filesystem::is_regular_file(tag.thumbnailPath()));
            CHECK(CountPngFiles(exportDir) == 2);
            CHECK(CountFullPngs(exportDir) == 1);
        }
    }
}

TEST_CASE("CoverFormatMatrix: sidecar fallback is mode-aware on non-MP3 formats", "[cover][mode][sidecar][matrix]")
{
    const std::filesystem::path root = CaseRoot("sidecar-matrix");
    REQUIRE(PrepareRoot(root));

    const FormatCase *formats[] = {
        &kFormats[0], // flac
        &kFormats[7], // matroska
    };
    for (const FormatCase *format : formats)
    {
        INFO("format=" << format->name);
        const std::filesystem::path subRoot = root / format->name;
        REQUIRE(std::filesystem::create_directory(subRoot));
        const std::filesystem::path basePath = subRoot / (format->name == "flac" ? "base.flac" : "base.mka");

        const bool generated = format->name == "flac"
                                   ? tagreader_test_support::GenerateFlacAudioSample(basePath)
                                   : tagreader_test_support::GenerateMatroskaAudioSample(basePath);
        REQUIRE(generated);
        const std::filesystem::path samplePath = subRoot / format->fileName;
        std::error_code ec;
        std::filesystem::copy_file(basePath, samplePath, std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(tagreader_test_support::WriteBinaryFile(subRoot / "cover.jpg",
                                                        tagreader_test_support::OneByOnePng()));

        {
            const std::filesystem::path exportDir = subRoot / "export-default";
            const MusicTag tag = TagReader::Read(samplePath, exportDir);
            REQUIRE_FALSE(tag.coverPath().empty());
            REQUIRE_FALSE(tag.thumbnailPath().empty());
            CHECK(CountPngFiles(exportDir) == 2);
            CHECK(CountFullPngs(exportDir) == 1);
        }

        {
            CoverProcessingOptions options;
            options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;
            const std::filesystem::path exportDir = subRoot / "export-thumb";
            const MusicTag tag = TagReader::Read(samplePath, exportDir, options);
            CHECK(tag.coverPath().empty());
            REQUIRE_FALSE(tag.thumbnailPath().empty());
            CHECK(CountPngFiles(exportDir) == 1);
            CHECK(CountFullPngs(exportDir) == 0);
        }
    }
}
