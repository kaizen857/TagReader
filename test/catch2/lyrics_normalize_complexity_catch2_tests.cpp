#include "src/text/TextNormalize.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using tagreader_core::RawLyrics;
using tagreader_text::NormalizeLyrics;

constexpr std::size_t kExpectedLyricLineCap = 20000;

std::string PaddedIndex(std::size_t value)
{
    std::string digits = std::to_string(value);
    if (digits.size() < 10)
    {
        digits.insert(digits.begin(), 10 - digits.size(), '0');
    }
    return digits;
}

RawLyrics BuildSameTimestampUniqueLyrics(std::size_t lineCount)
{
    RawLyrics lyrics{};
    lyrics.timedLines.reserve(lineCount);
    constexpr auto timestamp = std::chrono::microseconds(1234567);
    for (std::size_t index = 0; index < lineCount; ++index)
    {
        lyrics.timedLines.emplace_back(timestamp,
                                      "same timestamp lyric with shared comparison prefix " + PaddedIndex(index));
    }
    return lyrics;
}

std::chrono::nanoseconds TimeNormalizeSameTimestampUniqueLyrics(std::size_t lineCount)
{
    RawLyrics lyrics = BuildSameTimestampUniqueLyrics(lineCount);
    const auto start = std::chrono::steady_clock::now();
    NormalizeLyrics(lyrics);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(lyrics.timedLines.size() == lineCount);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
}
}

TEST_CASE("LyricsNormalize preserves semantics", "[LyricsNormalize][lyrics-normalize]")
{
    RawLyrics lyrics{};
    lyrics.text = "  plain lyric text  ";
    lyrics.timedLines = {
        {std::chrono::milliseconds(500), " same text "},
        {std::chrono::milliseconds(200), "alpha"},
        {std::chrono::milliseconds(200), " alpha "},
        {std::chrono::milliseconds(200), "beta"},
        {std::chrono::milliseconds(300), "alpha"},
        {std::chrono::milliseconds(400), "   "},
        {std::chrono::milliseconds(100), std::string(1, static_cast<char>(0xFF))},
    };

    NormalizeLyrics(lyrics);

    REQUIRE(lyrics.text == "plain lyric text");
    REQUIRE(lyrics.timedLines.size() == 4);
    REQUIRE(lyrics.timedLines[0].first == std::chrono::milliseconds(200));
    REQUIRE(lyrics.timedLines[0].second == "alpha");
    REQUIRE(lyrics.timedLines[1].first == std::chrono::milliseconds(200));
    REQUIRE(lyrics.timedLines[1].second == "beta");
    REQUIRE(lyrics.timedLines[2].first == std::chrono::milliseconds(300));
    REQUIRE(lyrics.timedLines[2].second == "alpha");
    REQUIRE(lyrics.timedLines[3].first == std::chrono::milliseconds(500));
    REQUIRE(lyrics.timedLines[3].second == "same text");
}

TEST_CASE("LyricsNormalize keeps the lyric line cap", "[LyricsNormalize][lyrics-normalize]")
{
    RawLyrics lyrics{};
    lyrics.timedLines.reserve(kExpectedLyricLineCap + 5);
    for (std::size_t index = 0; index < kExpectedLyricLineCap + 5; ++index)
    {
        lyrics.timedLines.emplace_back(std::chrono::microseconds(static_cast<long long>(index)),
                                      "cap line " + PaddedIndex(index));
    }

    NormalizeLyrics(lyrics);

    REQUIRE(lyrics.timedLines.size() == kExpectedLyricLineCap);
    REQUIRE(lyrics.timedLines.front().second == "cap line 0000000000");
    REQUIRE(lyrics.timedLines.back().second == "cap line 0000019999");
}

TEST_CASE("LyricsNormalize scales conservatively for same timestamps", "[LyricsNormalize][lyrics-normalize]")
{
    constexpr std::size_t kSmallLineCount = 2048;
    constexpr std::size_t kLargeLineCount = 8192;
    constexpr double kMaxExpectedRatio = 10.0;

    (void)TimeNormalizeSameTimestampUniqueLyrics(256);
    const auto small = TimeNormalizeSameTimestampUniqueLyrics(kSmallLineCount);
    const auto large = TimeNormalizeSameTimestampUniqueLyrics(kLargeLineCount);

    const double smallNs = static_cast<double>(small.count());
    const double largeNs = static_cast<double>(large.count());
    const double ratio = largeNs / std::max(1.0, smallNs);

    std::ostringstream message;
    message << "same-timestamp unique lyric normalization scaled too poorly: "
            << kSmallLineCount << " lines took " << small.count() << "ns, "
            << kLargeLineCount << " lines took " << large.count() << "ns, ratio=" << ratio
            << ", expected ratio below " << kMaxExpectedRatio;

    REQUIRE(std::isfinite(ratio));
    REQUIRE(ratio < kMaxExpectedRatio);
}
