#include "text/TextNormalize.hpp"

#include "text/TextCodec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace tagreader_text
{
using tagreader_core::RawLyrics;
using tagreader_core::RawMetadata;

namespace
{
constexpr std::size_t kMaxLyricLines = 20000;
constexpr std::size_t kMaxLrcTimestampsPerLine = 32;
constexpr std::size_t kMaxPlainLyricsBytes = 1z * 1024 * 1024;

void AssignDecoded(std::string &field, std::string value)
{
    field = std::move(value);
}

bool NormalizeAlreadyUtf8Field(std::string &value)
{
    value = TrimText(std::move(value));
    return IsValidUtf8(value);
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ParseDecimalU16Strict(std::string_view text, uint16_t &value)
{
    if (text.empty())
    {
        return false;
    }

    uint32_t result = 0;
    for (char ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10)
        {
            return false;
        }
        result = result * 10 + digit;
        if (result > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }
    }

    value = static_cast<uint16_t>(result);
    return true;
}

bool IsLrcMetadataLine(std::string_view line)
{
    if (line.size() < 4 || line.front() != '[')
    {
        return false;
    }

    const std::size_t close = line.find(']');
    if (close == std::string_view::npos)
    {
        return false;
    }

    const std::string_view token = line.substr(1, close - 1);
    const std::size_t colon = token.find(':');
    if (colon == std::string_view::npos)
    {
        return false;
    }

    const std::string key = ToLower(std::string(token.substr(0, colon)));
    return key == "ar" || key == "ti" || key == "al" || key == "by" || key == "offset" ||
           key == "au" || key == "length" || key == "re" || key == "ve";
}

void AppendPlainLyrics(RawLyrics &lyrics, std::string text)
{
    text = TrimText(std::move(text));
    if (!text.empty())
    {
        lyrics.text = std::move(text);
    }
}

bool ParseLrcTimestamp(std::string_view token, std::chrono::microseconds &timestamp)
{
    const auto close = token.find(']');
    if (token.empty() || token.front() != '[' || close == std::string_view::npos)
    {
        return false;
    }

    const std::string timePart = std::string(token.substr(1, close - 1));
    const auto colon = timePart.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }

    uint16_t minutes = 0;
    if (!ParseDecimalU16Strict(timePart.substr(0, colon), minutes))
    {
        return false;
    }

    constexpr uint16_t kMaxLrcMinutes = 999;
    if (minutes > kMaxLrcMinutes)
    {
        return false;
    }

    const std::string secondsPart = timePart.substr(colon + 1);
    const auto dot = secondsPart.find('.');
    uint16_t seconds = 0;
    const std::string_view secondsText = dot == std::string::npos ? std::string_view(secondsPart) : std::string_view(secondsPart).substr(0, dot);
    if (!ParseDecimalU16Strict(secondsText, seconds) || seconds >= 60)
    {
        return false;
    }

    uint16_t millis = 0;
    if (dot != std::string::npos)
    {
        std::string_view frac = std::string_view(secondsPart).substr(dot + 1);
        if (frac.empty() || frac.size() > 3)
        {
            return false;
        }
        uint16_t fraction = 0;
        if (!ParseDecimalU16Strict(frac, fraction))
        {
            return false;
        }
        millis = fraction;
        if (frac.size() == 1)
        {
            millis = static_cast<uint16_t>(fraction * 100);
        }
        else if (frac.size() == 2)
        {
            millis = static_cast<uint16_t>(fraction * 10);
        }
    }

    timestamp = std::chrono::minutes(minutes) + std::chrono::seconds(seconds) + std::chrono::milliseconds(millis);
    return true;
}
}

void NormalizeMetadata(RawMetadata &metadata)
{
    constexpr std::size_t kMaxFinalTextFieldBytes = 65536;

    auto normalize = [kMaxFinalTextFieldBytes](std::string &text)
    {
        std::string normalized = std::move(text);
        normalized = TrimText(std::move(normalized));
        if (normalized.size() > kMaxFinalTextFieldBytes)
        {
            std::size_t cut = kMaxFinalTextFieldBytes;
            while (cut > 0 && (static_cast<unsigned char>(normalized[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            normalized.resize(cut);
        }
        if (IsValidUtf8(normalized))
        {
            AssignDecoded(text, std::move(normalized));
        }
        else
        {
            text.clear();
        }
    };

    normalize(metadata.title);
    normalize(metadata.genre);
    normalize(metadata.artist);
    normalize(metadata.album);
    normalize(metadata.albumArtist);
    normalize(metadata.composer);
}

void NormalizeLyrics(RawLyrics &lyrics)
{
    if (!lyrics.text.empty())
    {
        if (!NormalizeAlreadyUtf8Field(lyrics.text))
        {
            lyrics.text.clear();
        }
    }

    for (auto &line : lyrics.timedLines)
    {
        if (!NormalizeAlreadyUtf8Field(line.second))
        {
            line.second.clear();
        }
    }

    lyrics.timedLines.erase(std::remove_if(lyrics.timedLines.begin(), lyrics.timedLines.end(), [](const auto &line)
                                           { return line.second.empty(); }),
                            lyrics.timedLines.end());
    if (lyrics.timedLines.size() > kMaxLyricLines)
    {
        lyrics.timedLines.resize(kMaxLyricLines);
    }
    std::stable_sort(lyrics.timedLines.begin(), lyrics.timedLines.end(), [](const auto &lhs, const auto &rhs)
                     { return lhs.first < rhs.first; });
    for (auto groupBegin = lyrics.timedLines.begin(); groupBegin != lyrics.timedLines.end();)
    {
        const auto timestamp = groupBegin->first;
        auto groupEnd = std::find_if(groupBegin, lyrics.timedLines.end(), [timestamp](const auto &line)
                                     { return line.first != timestamp; });
        auto write = groupBegin;
        for (auto read = groupBegin; read != groupEnd; ++read)
        {
            const bool duplicate = std::any_of(groupBegin, write, [&read](const auto &line)
                                               { return line.second == read->second; });
            if (!duplicate)
            {
                if (write != read)
                {
                    *write = std::move(*read);
                }
                ++write;
            }
        }
        groupBegin = lyrics.timedLines.erase(write, groupEnd);
    }
}

void ReadLyricsFromPlainText(RawLyrics &lyrics, std::string_view text)
{
    if (text.size() > kMaxPlainLyricsBytes)
    {
        return;
    }

    std::string plain;
    std::vector<std::pair<std::chrono::microseconds, std::string>> timed;

    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find_first_of("\r\n", start);
        const std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);

        if (!line.empty())
        {
            if (IsLrcMetadataLine(line))
            {
                if (end == std::string_view::npos)
                {
                    break;
                }

                start = end + 1;
                while (start < text.size() && (text[start] == '\r' || text[start] == '\n'))
                {
                    ++start;
                }
                continue;
            }

            std::size_t scan = 0;
            std::array<std::chrono::microseconds, kMaxLrcTimestampsPerLine> timestamps{};
            std::size_t timestampCount = 0;
            while (scan < line.size() && line[scan] == '[')
            {
                const std::size_t close = line.find(']', scan);
                if (close == std::string_view::npos)
                {
                    break;
                }

                std::chrono::microseconds ts{};
                if (!ParseLrcTimestamp(line.substr(scan, close - scan + 1), ts))
                {
                    break;
                }

                if (timestampCount < timestamps.size())
                {
                    timestamps[timestampCount++] = ts;
                }
                scan = close + 1;
            }

            if (timestampCount > 0)
            {
                const std::string lyricText = TrimText(std::string(line.substr(scan)));
                if (!lyricText.empty())
                {
                    const std::size_t remainingLines = kMaxLyricLines - timed.size();
                    const std::size_t linesToAppend = std::min(timestampCount, remainingLines);
                    for (std::size_t index = 0; index < linesToAppend; ++index)
                    {
                        timed.emplace_back(timestamps[index], lyricText);
                    }
                }
            }
            else
            {
                if (!plain.empty())
                {
                    plain.push_back('\n');
                }
                plain.append(std::string(line));
            }
        }

        if (timed.size() >= kMaxLyricLines)
        {
            break;
        }

        if (end == std::string_view::npos)
        {
            break;
        }

        start = end + 1;
        while (start < text.size() && (text[start] == '\r' || text[start] == '\n'))
        {
            ++start;
        }
    }

    if (!timed.empty())
    {
        lyrics.timedLines = std::move(timed);
    }
    else
    {
        if (!plain.empty())
        {
            AppendPlainLyrics(lyrics, std::move(plain));
        }
    }
}
}
