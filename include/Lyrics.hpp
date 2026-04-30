#ifndef LYRIC_HPP
#define LYRIC_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Lyric
{
private:
    std::chrono::microseconds timestamp_{};
    std::string text_;

public:
    Lyric() = default;
    Lyric(std::chrono::microseconds timestamp, std::string text) : timestamp_(timestamp), text_(std::move(text))
    {
    }

    std::chrono::microseconds timestamp() const noexcept
    { return timestamp_; }
    void setTimestamp(std::chrono::microseconds timestamp) noexcept
    { timestamp_ = timestamp; }

    std::string_view text() const noexcept
    { return text_; }
    void setText(std::string_view text)
    { text_ = text; }
};

class Lyrics
{
private:
    std::vector<Lyric> lyrics_;

public:
    Lyrics() = default;
    explicit Lyrics(std::vector<Lyric> lyrics) : lyrics_(std::move(lyrics))
    {
    }

    const std::vector<Lyric> &lyrics() const noexcept
    { return lyrics_; }
    void setLyrics(std::vector<Lyric> lyrics)
    { lyrics_ = std::move(lyrics); }

    void addLyric(const Lyric &lyric)
    { lyrics_.push_back(lyric); }
    void addLyric(Lyric &&lyric)
    { lyrics_.push_back(std::move(lyric)); }

    void clear() noexcept
    { lyrics_.clear(); }
    bool empty() const noexcept
    { return lyrics_.empty(); }
    std::size_t size() const noexcept
    { return lyrics_.size(); }
};

#endif // LYRIC_HPP
