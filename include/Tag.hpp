#ifndef __TAG__HPP__
#define __TAG__HPP__

#include <boost/flyweight.hpp>
#include <cstdint>
#include <filesystem>

class MusicTag
{
    using FlyStr = boost::flyweight<std::string>;

private:
    // 曲目元数据信息
    FlyStr title;         // 标题
    FlyStr genre;         // 流派
    FlyStr artist;        // 艺术家
    FlyStr album;         // 专辑
    FlyStr albumArtist;   // 专辑艺术家
    FlyStr composer;      // 作曲家
    uint16_t year;        // 年份
    uint16_t trackNumber; // 音轨
    uint16_t discNumber;  // 光盘

    // 文件信息
    std::filesystem::path filePath;               // 文件路径
    std::filesystem::path coverPath;              // 封面路径
    int64_t duration;                             // 时长(单位微秒)
    int64_t offset;                               // 偏移量(单位微秒)
    std::filesystem::file_time_type lastModified; // 最后修改时间
    uint32_t sampleRate;                          // 采样率
    uint32_t bitDepth;                            // 比特深度
    uint32_t bitRate;                             // 比特率
    uint8_t channels;                             // 声道数
    FlyStr format;                                // 音频格式

    // 播放统计
    uint32_t playCount;                               // 播放次数
    uint8_t rating;                                   // 评分
    std::chrono::system_clock::time_point lastPlayed; // 最后播放时间
};

#endif