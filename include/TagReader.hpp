#ifndef __TAGREADER_HPP__
#define __TAGREADER_HPP__

#include "Tag.hpp"
#include <filesystem>

class TagReader
{
public:
    static MusicTag Read(const std::filesystem::path &filePath);
    static MusicTag Read(const std::filesystem::path &filePath, const std::filesystem::path &coverExportDir);
};

#endif
