#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <filesystem>
#include <vector>

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);
    static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
    static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath);
    static std::vector<MusicTag> ReadCueSheet(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
};

#endif
