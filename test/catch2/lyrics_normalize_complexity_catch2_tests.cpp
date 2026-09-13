#include "src/text/TextNormalize.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <iostream>
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

std::chrono::nanoseconds CpuTicksToNanoseconds(std::clock_t ticks)
{
    constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
    return std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(
        static_cast<double>(ticks) * kNanosecondsPerSecond / static_cast<double>(CLOCKS_PER_SEC)));
}

struct NormalizeTiming
{
    // 判定口径: 进程 CPU 时间(std::clock), 只累计实际执行, 不计入被抢占的等待。
    std::chrono::nanoseconds cpu = std::chrono::nanoseconds::max();
    // 诊断信息: 墙钟最小值。深饥饿下会被调度分布污染(实测虚高约 100 倍), 仅用于失败
    // 信息里区分"算法变慢"与"机器被抢占", 不参与断言。
    std::chrono::nanoseconds wall = std::chrono::nanoseconds::max();
};

NormalizeTiming TimeNormalizeSameTimestampUniqueLyrics(std::size_t lineCount, int attempts = 50)
{
    // 计时口径改为 CPU 时间而非墙钟。深饥饿(例如 8 个 nice-0 忙循环 + nice-19 测试进程)
    // 下, 墙钟 min-of-N 采样的是调度分布左尾而非计算时间: 2048 行偶尔获得一段未被打断的
    // 切片, 8192 行(4x 工作量)拿不到同等干净的切片, 比值虚高约 100 倍(实测正常 4.4,
    // 深饥饿 602)导致与实现无关的确定性失败。CPU 时间排除被抢占等待后不再随调度公平性
    // 漂移, 而 min-of-N 仍用于压掉单次样本的测量噪声与缓存抖动; 它同时保留区分 O(n)/O(n²)
    // 的能力(正常线性比值 4.4, 二次实现实测 16.1, 阈值 10 仍可拦截)。
    NormalizeTiming best{};
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        RawLyrics lyrics = BuildSameTimestampUniqueLyrics(lineCount);
        const std::clock_t cpuStart = std::clock();
        const auto wallStart = std::chrono::steady_clock::now();
        NormalizeLyrics(lyrics);
        const auto wallElapsed = std::chrono::steady_clock::now() - wallStart;
        const std::clock_t cpuEnd = std::clock();

        if (cpuStart != static_cast<std::clock_t>(-1) && cpuEnd != static_cast<std::clock_t>(-1))
        {
            best.cpu = std::min(best.cpu, CpuTicksToNanoseconds(cpuEnd - cpuStart));
        }
        best.wall = std::min(best.wall, std::chrono::duration_cast<std::chrono::nanoseconds>(wallElapsed));
        REQUIRE(lyrics.timedLines.size() == lineCount);
    }
    return best;
}

// CPU 时间口径下的噪声地板: std::clock 在 Linux/macOS 上是微秒级分辨率的进程 CPU 时间,
// 深饥饿不计入被抢占等待, 计时只受测量分辨率与缓存抖动影响。100µs 远高于计时分辨率,
// 又远低于任何真实机器上 2048 行归一化的 CPU 耗时(本机约 1.7ms), 因此正常/饥饿机器都
// 不会因此跳过比值断言; 该地板仅防御计时完全失效的退化环境。
constexpr std::chrono::nanoseconds kComplexityCpuNoiseFloor = std::chrono::microseconds(100);
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
    auto small = TimeNormalizeSameTimestampUniqueLyrics(kSmallLineCount);
    auto large = TimeNormalizeSameTimestampUniqueLyrics(kLargeLineCount);

    if (small.cpu < kComplexityCpuNoiseFloor || large.cpu < kComplexityCpuNoiseFloor)
    {
        // 低于噪声地板说明计时不可信; 用更多样本重测一次(取最小只会更接近真实值),
        // 给恰好卡在地板附近的慢速机器一次公平机会。
        small = TimeNormalizeSameTimestampUniqueLyrics(kSmallLineCount, 250);
        large = TimeNormalizeSameTimestampUniqueLyrics(kLargeLineCount, 250);
    }

    // 比值只使用 CPU 时间: 深饥饿不会抬高该口径(正常与饥饿实测均约 4.3-4.4),
    // 而墙钟口径在同样条件下会虚高约 100 倍。墙钟仅作为诊断字段输出。
    const double smallNs = static_cast<double>(small.cpu.count());
    const double largeNs = static_cast<double>(large.cpu.count());
    const double ratio = largeNs / std::max(1.0, smallNs);

    std::ostringstream message;
    message << "same-timestamp unique lyric normalization scaled too poorly: "
            << kSmallLineCount << " lines cpu=" << small.cpu.count() << "ns wall=" << small.wall.count()
            << "ns, " << kLargeLineCount << " lines cpu=" << large.cpu.count() << "ns wall=" << large.wall.count()
            << "ns, cpu_ratio=" << ratio << ", expected cpu ratio below " << kMaxExpectedRatio;

    if (small.cpu < kComplexityCpuNoiseFloor || large.cpu < kComplexityCpuNoiseFloor)
    {
        // 计时低于地板说明测量不可信(见上方地板说明), 此时比值断言本身不可靠; 结构断言
        // 仍已执行, 这里仅跳过比值断言并说明原因。CPU 口径下正常/饥饿机器都不会触发。
        std::cerr << "LyricsNormalize complexity ratio skipped (cpu measurement below noise floor): "
                  << message.str() << '\n';
        return;
    }

    REQUIRE(std::isfinite(ratio));
    REQUIRE(ratio < kMaxExpectedRatio);
}
