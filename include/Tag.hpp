#ifndef __TAG__HPP__
#define __TAG__HPP__

#include "Lyrics.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

class MusicTag
{
private:
    // 曲目元数据信息
    std::string title_;      // 标题
    std::string genre_;      // 流派
    std::string artist_;     // 艺术家
    std::string album_;      // 专辑
    std::string albumArtist_; // 专辑艺术家
    std::string composer_;   // 作曲家
    uint16_t year_{};        // 年份
    uint16_t trackNumber_{}; // 音轨
    uint16_t discNumber_{};  // 光盘
    Lyrics lyrics_{};        // 歌词

    // 文件信息
    std::filesystem::path filePath_{};               // 文件路径
    std::filesystem::path coverPath_{};              // 封面路径
    std::filesystem::path thumbnailPath_{};          // 缩略图路径
    int64_t duration_{};                             // 时长(单位微秒)
    int64_t offset_{};                               // 偏移量(单位微秒)
    std::filesystem::file_time_type lastModified_{}; // 最后修改时间
    uint32_t sampleRate_{};                          // 采样率
    uint32_t bitDepth_{};                            // 比特深度
    uint32_t bitRate_{};                             // 比特率
    uint8_t channels_{};                             // 声道数
    std::string format_{};                           // 音频格式

    // 播放统计
    uint32_t playCount_{};                               // 播放次数
    uint8_t rating_{};                                   // 评分
    std::chrono::system_clock::time_point lastPlayed_{}; // 最后播放时间

public:
    MusicTag() = default;

    const std::string &title() const noexcept
    { return title_; }
    void setTitle(std::string value)
    { title_ = std::move(value); }

    const std::string &genre() const noexcept
    { return genre_; }
    void setGenre(std::string value)
    { genre_ = std::move(value); }

    const std::string &artist() const noexcept
    { return artist_; }
    void setArtist(std::string value)
    { artist_ = std::move(value); }

    const std::string &album() const noexcept
    { return album_; }
    void setAlbum(std::string value)
    { album_ = std::move(value); }

    const std::string &albumArtist() const noexcept
    { return albumArtist_; }
    void setAlbumArtist(std::string value)
    { albumArtist_ = std::move(value); }

    const std::string &composer() const noexcept
    { return composer_; }
    void setComposer(std::string value)
    { composer_ = std::move(value); }

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

    const std::filesystem::path &thumbnailPath() const noexcept
    { return thumbnailPath_; }
    void setThumbnailPath(std::filesystem::path value)
    { thumbnailPath_ = std::move(value); }

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

    const std::string &format() const noexcept
    { return format_; }
    void setFormat(std::string value)
    { format_ = std::move(value); }

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
