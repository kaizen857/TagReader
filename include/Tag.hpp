#ifndef __TAG__HPP__
#define __TAG__HPP__

#include "Lyrics.hpp"
#include <boost/flyweight.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

class MusicTag
{
    using FlyStr = boost::flyweight<std::string>;

private:
    // 曲目元数据信息
    FlyStr title_;           // 标题
    FlyStr genre_;           // 流派
    FlyStr artist_;          // 艺术家
    FlyStr album_;           // 专辑
    FlyStr albumArtist_;     // 专辑艺术家
    FlyStr composer_;        // 作曲家
    uint16_t year_{};        // 年份
    uint16_t trackNumber_{}; // 音轨
    uint16_t discNumber_{};  // 光盘
    Lyrics lyrics_{};        // 歌词

    // 文件信息
    std::filesystem::path filePath_{};               // 文件路径
    std::filesystem::path coverPath_{};              // 封面路径
    int64_t duration_{};                             // 时长(单位微秒)
    int64_t offset_{};                               // 偏移量(单位微秒)
    std::filesystem::file_time_type lastModified_{}; // 最后修改时间
    uint32_t sampleRate_{};                          // 采样率
    uint32_t bitDepth_{};                            // 比特深度
    uint32_t bitRate_{};                             // 比特率
    uint8_t channels_{};                             // 声道数
    FlyStr format_{};                                // 音频格式

    // 播放统计
    uint32_t playCount_{};                               // 播放次数
    uint8_t rating_{};                                   // 评分
    std::chrono::system_clock::time_point lastPlayed_{}; // 最后播放时间

public:
    MusicTag() = default;

    std::string_view title() const noexcept
    { return title_.get(); }
    void setTitle(std::string_view value)
    { title_ = std::string(value); }

    std::string_view genre() const noexcept
    { return genre_.get(); }
    void setGenre(std::string_view value)
    { genre_ = std::string(value); }

    std::string_view artist() const noexcept
    { return artist_.get(); }
    void setArtist(std::string_view value)
    { artist_ = std::string(value); }

    std::string_view album() const noexcept
    { return album_.get(); }
    void setAlbum(std::string_view value)
    { album_ = std::string(value); }

    std::string_view albumArtist() const noexcept
    { return albumArtist_.get(); }
    void setAlbumArtist(std::string_view value)
    { albumArtist_ = std::string(value); }

    std::string_view composer() const noexcept
    { return composer_.get(); }
    void setComposer(std::string_view value)
    { composer_ = std::string(value); }

    uint16_t year() const noexcept
    { return year_; }
    void setYear(uint16_t value) noexcept
    { year_ = value; }

    uint16_t trackNumber() const noexcept
    { return trackNumber_; }
    void setTrackNumber(uint16_t value) noexcept
    { trackNumber_ = value; }

    uint16_t discNumber() const noexcept
    { return discNumber_; }
    void setDiscNumber(uint16_t value) noexcept
    { discNumber_ = value; }

    const Lyrics &lyrics() const noexcept
    { return lyrics_; }
    Lyrics &lyrics() noexcept
    { return lyrics_; }
    void setLyrics(Lyrics value)
    { lyrics_ = std::move(value); }

    const std::filesystem::path &filePath() const noexcept
    { return filePath_; }
    void setFilePath(std::filesystem::path value)
    { filePath_ = std::move(value); }

    const std::filesystem::path &coverPath() const noexcept
    { return coverPath_; }
    void setCoverPath(std::filesystem::path value)
    { coverPath_ = std::move(value); }

    int64_t duration() const noexcept
    { return duration_; }
    void setDuration(int64_t value) noexcept
    { duration_ = value; }

    int64_t offset() const noexcept
    { return offset_; }
    void setOffset(int64_t value) noexcept
    { offset_ = value; }

    std::filesystem::file_time_type lastModified() const noexcept
    { return lastModified_; }
    void setLastModified(std::filesystem::file_time_type value) noexcept
    { lastModified_ = value; }

    uint32_t sampleRate() const noexcept
    { return sampleRate_; }
    void setSampleRate(uint32_t value) noexcept
    { sampleRate_ = value; }

    uint32_t bitDepth() const noexcept
    { return bitDepth_; }
    void setBitDepth(uint32_t value) noexcept
    { bitDepth_ = value; }

    uint32_t bitRate() const noexcept
    { return bitRate_; }
    void setBitRate(uint32_t value) noexcept
    { bitRate_ = value; }

    uint8_t channels() const noexcept
    { return channels_; }
    void setChannels(uint8_t value) noexcept
    { channels_ = value; }

    std::string_view format() const noexcept
    { return format_.get(); }
    void setFormat(std::string_view value)
    { format_ = std::string(value); }

    uint32_t playCount() const noexcept
    { return playCount_; }
    void setPlayCount(uint32_t value) noexcept
    { playCount_ = value; }

    uint8_t rating() const noexcept
    { return rating_; }
    void setRating(uint8_t value) noexcept
    { rating_ = value; }

    std::chrono::system_clock::time_point lastPlayed() const noexcept
    { return lastPlayed_; }
    void setLastPlayed(std::chrono::system_clock::time_point value) noexcept
    { lastPlayed_ = value; }
};

#endif
