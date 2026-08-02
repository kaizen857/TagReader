#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"
#include "TagReader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view kLyricsText = "first lyric line\nsecond lyric line\n";

struct TrackSnapshot
{
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t offset{};
    std::int64_t duration{};
    std::filesystem::path filePath;
    std::filesystem::path coverPath;
    std::filesystem::path thumbnailPath;
    std::vector<std::pair<std::chrono::microseconds, std::string>> lyrics;
};

bool SameTrackContent(const TrackSnapshot &left, const TrackSnapshot &right)
{
    return left.title == right.title && left.artist == right.artist && left.album == right.album &&
           left.offset == right.offset && left.duration == right.duration && left.lyrics == right.lyrics;
}

bool SameTrack(const TrackSnapshot &left, const TrackSnapshot &right)
{
    return SameTrackContent(left, right) && left.filePath == right.filePath;
}

TrackSnapshot Snapshot(const MusicTag &tag)
{
    TrackSnapshot snapshot;
    snapshot.title = tag.title();
    snapshot.artist = tag.artist();
    snapshot.album = tag.album();
    snapshot.offset = tag.offset();
    snapshot.duration = tag.duration();
    snapshot.filePath = tag.filePath();
    snapshot.coverPath = tag.coverPath();
    snapshot.thumbnailPath = tag.thumbnailPath();
    for (const Lyric &line : tag.lyrics().lyrics())
    {
        snapshot.lyrics.emplace_back(line.timestamp(), std::string(line.text()));
    }
    return snapshot;
}

std::vector<TrackSnapshot> SnapshotAll(const std::vector<MusicTag> &tags)
{
    std::vector<TrackSnapshot> snapshots;
    snapshots.reserve(tags.size());
    for (const MusicTag &tag : tags)
    {
        snapshots.push_back(Snapshot(tag));
    }
    return snapshots;
}

bool EnsureCleanRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

std::vector<std::uint8_t> ApicPayload(const std::vector<std::uint8_t> &imageBytes)
{
    std::vector<std::uint8_t> payload{0};
    tagreader_test_support::AppendBytes(payload, "image/png");
    payload.insert(payload.end(), {0, 3, 0});
    payload.insert(payload.end(), imageBytes.begin(), imageBytes.end());
    return payload;
}

// ID3v2.3 frames: TIT2 + optional APIC + USLT. The audio file carries no
// embedded art when withCover is false; corruptCover makes the APIC payload
// undecodable so cover processing fails while metadata/lyrics stay readable.
std::vector<std::uint8_t> Id3Frames(bool withCover, bool corruptCover)
{
    std::vector<std::uint8_t> titlePayload{0};
    tagreader_test_support::AppendBytes(titlePayload, "AudioTitle");
    std::vector<std::uint8_t> usltPayload{0, 'e', 'n', 'g', 0};
    tagreader_test_support::AppendBytes(usltPayload, kLyricsText);

    std::vector<std::uint8_t> frames = tagreader_test_support::BuildId3v23Frame("TIT2", titlePayload);
    if (withCover)
    {
        const std::vector<std::uint8_t> image = corruptCover
            ? std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
            : tagreader_test_support::OneByOnePng();
        const auto apicFrame = tagreader_test_support::BuildId3v23Frame("APIC", ApicPayload(image));
        frames.insert(frames.end(), apicFrame.begin(), apicFrame.end());
    }
    const auto usltFrame = tagreader_test_support::BuildId3v23Frame("USLT", usltPayload);
    frames.insert(frames.end(), usltFrame.begin(), usltFrame.end());
    return frames;
}

bool WriteTaggedMp3(const std::filesystem::path &baseMp3, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &frames)
{
    const std::vector<std::uint8_t> baseBytes = tagreader_test_support::ReadBinaryFile(baseMp3);
    if (baseBytes.empty())
    {
        return false;
    }
    std::vector<std::uint8_t> output = tagreader_test_support::BuildId3v23Tag(frames);
    output.insert(output.end(), baseBytes.begin(), baseBytes.end());
    return tagreader_test_support::WriteBinaryFile(outputPath, output);
}

std::string SingleFileCue(std::string_view audioFileName)
{
    return "TITLE \"Cue Album\"\n"
           "PERFORMER \"Cue Artist\"\n"
           "FILE \"" + std::string(audioFileName) + "\" MP3\n"
           "  TRACK 01 AUDIO\n"
           "    TITLE \"Cue Track One\"\n"
           "    PERFORMER \"Cue Track Artist\"\n"
           "    INDEX 01 00:00:00\n"
           "  TRACK 02 AUDIO\n"
           "    TITLE \"Cue Track Two\"\n"
           "    INDEX 01 00:01:00\n";
}

std::string MultiFileCue(std::string_view firstAudio, std::string_view secondAudio)
{
    return "TITLE \"Cue Album\"\n"
           "FILE \"" + std::string(firstAudio) + "\" MP3\n"
           "  TRACK 01 AUDIO\n"
           "    TITLE \"First Track\"\n"
           "    INDEX 01 00:00:00\n"
           "FILE \"" + std::string(secondAudio) + "\" MP3\n"
           "  TRACK 02 AUDIO\n"
           "    TITLE \"Second Track\"\n"
           "    INDEX 01 00:00:00\n";
}
}

TEST_CASE("cue cover options flow into every referenced audio read", "[cue][cover][options]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_options_flow");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "audio.mp3", Id3Frames(true, false)));

    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, SingleFileCue("audio.mp3")));

    const std::vector<MusicTag> baseline = TagReader::ReadCueSheet(cuePath, root / "export-baseline");
    REQUIRE(baseline.size() == 2);

    CoverProcessingOptions defaults;
    const std::vector<MusicTag> withDefaults = TagReader::ReadCueSheet(cuePath, root / "export-defaults", defaults);
    REQUIRE(withDefaults.size() == 2);
    REQUIRE_FALSE(withDefaults.front().coverPath().empty());
    REQUIRE_FALSE(withDefaults.front().thumbnailPath().empty());

    CoverProcessingOptions noThumbnail;
    noThumbnail.generateThumbnail = false;
    const std::vector<MusicTag> withNoThumbnail = TagReader::ReadCueSheet(cuePath, root / "export-nothumb", noThumbnail);
    REQUIRE(withNoThumbnail.size() == 2);
    REQUIRE_FALSE(withNoThumbnail.front().coverPath().empty());
    REQUIRE(withNoThumbnail.front().thumbnailPath().empty());

    const std::vector<TrackSnapshot> baselineSnapshots = SnapshotAll(baseline);
    REQUIRE(SameTrack(SnapshotAll(withDefaults)[0], baselineSnapshots[0]));
    REQUIRE(SameTrack(SnapshotAll(withDefaults)[1], baselineSnapshots[1]));
    REQUIRE(SameTrack(SnapshotAll(withNoThumbnail)[0], baselineSnapshots[0]));
    REQUIRE(SameTrack(SnapshotAll(withNoThumbnail)[1], baselineSnapshots[1]));
}

TEST_CASE("cue cover options modes keep every track, timing and lyric equal to baseline", "[cue][cover][options][mode]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_options_modes");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "audio.mp3", Id3Frames(true, false)));

    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, SingleFileCue("audio.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(cuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);
    REQUIRE(baseline[0].offset == 0);
    REQUIRE(baseline[1].offset == 1000000);
    REQUIRE(baseline[0].duration == 1000000);
    REQUIRE_FALSE(baseline[0].lyrics.empty());
    REQUIRE(baseline[0].lyrics[0].first == std::chrono::microseconds{0});
    REQUIRE(baseline[0].lyrics[0].second == "first lyric line");

    const std::vector<CoverProcessingOptions::CoverProcessingMode> modes{
        CoverProcessingOptions::CoverProcessingMode::Disabled,
        CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly,
        CoverProcessingOptions::CoverProcessingMode::FullOnly,
        CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail,
    };
    for (const CoverProcessingOptions::CoverProcessingMode mode : modes)
    {
        INFO(static_cast<int>(mode));
        CoverProcessingOptions options;
        options.mode = mode;
        const std::vector<TrackSnapshot> snapshots = SnapshotAll(TagReader::ReadCueSheet(cuePath, root / ("export-mode-" + std::to_string(static_cast<int>(mode))), options));
        REQUIRE(snapshots.size() == baseline.size());
        REQUIRE(SameTrack(snapshots[0], baseline[0]));
        REQUIRE(SameTrack(snapshots[1], baseline[1]));
    }
}

TEST_CASE("cue cover options Ignore retains tracks for corrupt embedded art", "[cue][cover][options][ignore]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_ignore_corrupt");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "valid.mp3", Id3Frames(true, false)));
    REQUIRE(WriteTaggedMp3(basePath, root / "corrupt.mp3", Id3Frames(true, true)));

    const std::filesystem::path validCuePath = root / "valid.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(validCuePath, SingleFileCue("valid.mp3")));
    const std::filesystem::path corruptCuePath = root / "corrupt.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(corruptCuePath, SingleFileCue("corrupt.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(validCuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);

    CoverProcessingOptions options;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(corruptCuePath, root / "export-ignore", options);
    REQUIRE(tags.size() == baseline.size());
    for (std::size_t index = 0; index < tags.size(); ++index)
    {
        INFO(index);
        REQUIRE(SameTrackContent(Snapshot(tags[index]), baseline[index]));
        REQUIRE(tags[index].coverPath().empty());
        REQUIRE(tags[index].thumbnailPath().empty());
    }
}

TEST_CASE("cue cover options Ignore keeps every file when one referenced audio has corrupt art", "[cue][cover][options][ignore][multifile]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_ignore_multifile");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "good-a.mp3", Id3Frames(true, false)));
    REQUIRE(WriteTaggedMp3(basePath, root / "good-b.mp3", Id3Frames(true, false)));
    REQUIRE(WriteTaggedMp3(basePath, root / "bad-b.mp3", Id3Frames(true, true)));

    const std::filesystem::path baselineCuePath = root / "baseline.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(baselineCuePath, MultiFileCue("good-a.mp3", "good-b.mp3")));
    const std::filesystem::path corruptCuePath = root / "corrupt.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(corruptCuePath, MultiFileCue("good-a.mp3", "bad-b.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(baselineCuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);

    CoverProcessingOptions options;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(corruptCuePath, root / "export-ignore", options);
    REQUIRE(tags.size() == baseline.size());
    REQUIRE(SameTrackContent(Snapshot(tags[0]), baseline[0]));
    REQUIRE(SameTrackContent(Snapshot(tags[1]), baseline[1]));
    REQUIRE_FALSE(tags[0].coverPath().empty());
    REQUIRE(tags[1].coverPath().empty());
}

TEST_CASE("cue cover options Ignore retains tracks for oversize embedded art", "[cue][cover][options][ignore][budget]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_ignore_oversize");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "audio.mp3", Id3Frames(true, false)));

    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, SingleFileCue("audio.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(cuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);

    CoverProcessingOptions options;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    options.maxSourceCoverBytes = 1;
    const std::vector<MusicTag> tags = TagReader::ReadCueSheet(cuePath, root / "export-ignore", options);
    REQUIRE(tags.size() == baseline.size());
    for (std::size_t index = 0; index < tags.size(); ++index)
    {
        INFO(index);
        REQUIRE(SameTrackContent(Snapshot(tags[index]), baseline[index]));
    }
}

TEST_CASE("cue cover options Ignore keeps tracks and lyrics when sidecar art is present or corrupt", "[cue][cover][options][ignore][sidecar]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_ignore_sidecar");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "audio.mp3", Id3Frames(false, false)));

    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, SingleFileCue("audio.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(cuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);
    REQUIRE(baseline.front().coverPath.empty());
    REQUIRE_FALSE(baseline.front().lyrics.empty());

    CoverProcessingOptions options;
    options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "folder.png", tagreader_test_support::OneByOnePng()));
    const std::vector<MusicTag> withSidecar = TagReader::ReadCueSheet(cuePath, root / "export-sidecar", options);
    REQUIRE(withSidecar.size() == baseline.size());
    REQUIRE(SameTrackContent(Snapshot(withSidecar[0]), baseline[0]));
    REQUIRE(SameTrackContent(Snapshot(withSidecar[1]), baseline[1]));
    REQUIRE_FALSE(withSidecar[0].coverPath().empty());

    REQUIRE(tagreader_test_support::WriteBinaryFile(root / "folder.png", std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    const std::vector<MusicTag> withCorruptSidecar = TagReader::ReadCueSheet(cuePath, root / "export-corrupt-sidecar", options);
    REQUIRE(withCorruptSidecar.size() == baseline.size());
    for (std::size_t index = 0; index < withCorruptSidecar.size(); ++index)
    {
        INFO(index);
        REQUIRE(SameTrackContent(Snapshot(withCorruptSidecar[index]), baseline[index]));
        REQUIRE(withCorruptSidecar[index].coverPath().empty());
    }
}

TEST_CASE("cue cover options Propagate surfaces the cover failure", "[cue][cover][options][propagate]")
{
    const std::filesystem::path root = tagreader_test_support::TemporaryArtifactRoot("cue_cover_propagate");
    REQUIRE(EnsureCleanRoot(root));

    const std::filesystem::path basePath = root / "base.mp3";
    REQUIRE(tagreader_test_support::GenerateBaseMp3(basePath));
    REQUIRE(WriteTaggedMp3(basePath, root / "audio.mp3", Id3Frames(true, false)));

    const std::filesystem::path cuePath = root / "album.cue";
    REQUIRE(tagreader_test_support::WriteTextFile(cuePath, SingleFileCue("audio.mp3")));

    const std::vector<TrackSnapshot> baseline = SnapshotAll(TagReader::ReadCueSheet(cuePath, root / "export-baseline"));
    REQUIRE(baseline.size() == 2);

    const std::filesystem::path target = root / "target";
    REQUIRE(std::filesystem::create_directory(target));
    const std::filesystem::path link = root / "link";
    REQUIRE(tagreader_test_support::PrepareSymlink(target, link));

    CoverProcessingOptions options;
    REQUIRE(options.failurePolicy == CoverProcessingOptions::CoverFailurePolicy::Propagate);
    try
    {
        (void)TagReader::ReadCueSheet(cuePath, link, options);
        FAIL("ExportDirectoryUnavailable should propagate with default policy");
    }
    catch (const CoverProcessingError &ex)
    {
        CHECK(ex.code() == CoverErrorCode::ExportDirectoryUnavailable);
        REQUIRE(ex.path().has_value());
        CHECK(ex.path()->string() == link.string());
    }

    CoverProcessingOptions ignore;
    ignore.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
    const std::vector<MusicTag> ignored = TagReader::ReadCueSheet(cuePath, link, ignore);
    REQUIRE(ignored.size() == baseline.size());
    REQUIRE(SameTrackContent(Snapshot(ignored[0]), baseline[0]));
    REQUIRE(SameTrackContent(Snapshot(ignored[1]), baseline[1]));
    REQUIRE(ignored[0].coverPath().empty());
    REQUIRE(ignored[1].coverPath().empty());
}
