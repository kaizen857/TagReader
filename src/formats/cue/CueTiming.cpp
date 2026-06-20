#include "formats/cue/CueTiming.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace tagreader_cue
{
namespace
{
std::optional<std::chrono::microseconds> ParseCueIndexTime(const CueIndex &index)
{
    if (index.frame >= 75 || index.second >= 60)
    {
        return std::nullopt;
    }

    constexpr std::uint64_t kFramesPerSecond = 75;
    constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;

    const std::uint64_t minutes = static_cast<std::uint64_t>(index.minute);
    const std::uint64_t seconds = static_cast<std::uint64_t>(index.second);
    const std::uint64_t frames = static_cast<std::uint64_t>(index.frame);

    if (minutes > (std::numeric_limits<std::uint64_t>::max() - seconds) / 60ULL)
    {
        return std::nullopt;
    }

    const std::uint64_t totalSeconds = minutes * 60ULL + seconds;
    if (totalSeconds > (std::numeric_limits<std::uint64_t>::max() - frames) / kFramesPerSecond)
    {
        return std::nullopt;
    }

    const std::uint64_t totalFrames = totalSeconds * kFramesPerSecond + frames;
    const __int128 scaled = static_cast<__int128>(totalFrames) * static_cast<__int128>(kMicrosecondsPerSecond);
    if (scaled < 0)
    {
        return std::nullopt;
    }

    return std::chrono::microseconds{static_cast<std::int64_t>((scaled + (kFramesPerSecond / 2)) / kFramesPerSecond)};
}

std::vector<std::optional<std::chrono::microseconds>> CollectTrackOffsets(const CueFile &file)
{
    std::vector<std::optional<std::chrono::microseconds>> offsets;
    offsets.reserve(file.tracks.size());

    for (const auto &track : file.tracks)
    {
        std::optional<std::chrono::microseconds> offset;
        for (const auto &index : track.indexes)
        {
            if (index.number == 1)
            {
                offset = ParseCueIndexTime(index);
                break;
            }
        }
        offsets.push_back(offset);
    }

    return offsets;
}
}

bool ApplyCueTiming(const CueFile &file, std::vector<MusicTag> &tags)
{
    const std::vector<std::optional<std::chrono::microseconds>> offsets = CollectTrackOffsets(file);
    if (tags.size() != file.tracks.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < tags.size(); ++index)
    {
        const std::optional<std::chrono::microseconds> currentOffset = offsets[index];
        if (!currentOffset.has_value())
        {
            continue;
        }

        tags[index].setOffset(currentOffset->count());

        std::optional<std::chrono::microseconds> duration;
        if (index + 1 < tags.size())
        {
            const std::optional<std::chrono::microseconds> nextOffset = offsets[index + 1];
            if (nextOffset.has_value())
            {
                if (nextOffset->count() < currentOffset->count())
                {
                    return false;
                }
                duration = std::chrono::microseconds{nextOffset->count() - currentOffset->count()};
            }
        }
        else
        {
            const int64_t audioDuration = tags[index].duration();
            if (audioDuration >= currentOffset->count())
            {
                duration = std::chrono::microseconds{audioDuration - currentOffset->count()};
            }
        }

        if (duration.has_value())
        {
            tags[index].setDuration(duration->count());
        }
    }

    return true;
}
}
