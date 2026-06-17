#include "text/TextNormalize.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using tagreader_core::RawLyrics;
using tagreader_text::NormalizeLyrics;

constexpr std::size_t kExpectedLyricLineCap = 20000;

bool Expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "expectation failed: " << message << '\n';
        return false;
    }
    return true;
}

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
    if (lyrics.timedLines.size() != lineCount)
    {
        std::cerr << "unexpected deduplication for unique same-timestamp lines: expected " << lineCount
                  << " got " << lyrics.timedLines.size() << '\n';
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
}

bool SemanticsArePreserved()
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

    bool ok = true;
    ok &= Expect(lyrics.text == "plain lyric text", "plain lyrics should be trimmed and preserved");
    ok &= Expect(lyrics.timedLines.size() == 4, "empty, invalid, and duplicate timed lines should be removed");
    if (lyrics.timedLines.size() == 4)
    {
        ok &= Expect(lyrics.timedLines[0].first == std::chrono::milliseconds(200) && lyrics.timedLines[0].second == "alpha",
                     "first normalized timed line should be 200ms alpha");
        ok &= Expect(lyrics.timedLines[1].first == std::chrono::milliseconds(200) && lyrics.timedLines[1].second == "beta",
                     "same timestamp with different text should remain");
        ok &= Expect(lyrics.timedLines[2].first == std::chrono::milliseconds(300) && lyrics.timedLines[2].second == "alpha",
                     "same text at a different timestamp should remain");
        ok &= Expect(lyrics.timedLines[3].first == std::chrono::milliseconds(500) && lyrics.timedLines[3].second == "same text",
                     "later trimmed timed line should remain sorted by timestamp");
    }
    return ok;
}

bool LyricLineCapStillApplies()
{
    RawLyrics lyrics{};
    lyrics.timedLines.reserve(kExpectedLyricLineCap + 5);
    for (std::size_t index = 0; index < kExpectedLyricLineCap + 5; ++index)
    {
        lyrics.timedLines.emplace_back(std::chrono::microseconds(static_cast<long long>(index)),
                                      "cap line " + PaddedIndex(index));
    }

    NormalizeLyrics(lyrics);

    bool ok = true;
    ok &= Expect(lyrics.timedLines.size() == kExpectedLyricLineCap, "timed lyric normalization should keep the 20000 line cap");
    if (lyrics.timedLines.size() == kExpectedLyricLineCap)
    {
        ok &= Expect(lyrics.timedLines.front().second == "cap line 0000000000", "cap should preserve the first retained line");
        ok &= Expect(lyrics.timedLines.back().second == "cap line 0000019999", "cap should discard lines after the first 20000 entries");
    }
    return ok;
}

bool SameTimestampDedupScalesConservatively()
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

    std::cout << "same-timestamp scaling small_ns=" << small.count()
              << " large_ns=" << large.count()
              << " ratio=" << ratio << '\n';
    return Expect(std::isfinite(ratio) && ratio < kMaxExpectedRatio, message.str());
}
}

int main()
{
    const bool semanticsOk = SemanticsArePreserved();
    const bool capOk = LyricLineCapStillApplies();
    const bool complexityOk = SameTimestampDedupScalesConservatively();
    if (!semanticsOk || !capOk || !complexityOk)
    {
        return 1;
    }

    std::cout << "TagReaderLyricsNormalizeComplexityTests PASS\n";
    return 0;
}
