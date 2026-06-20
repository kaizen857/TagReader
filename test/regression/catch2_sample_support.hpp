#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace tagreader_test_support
{
std::vector<std::uint8_t> OneByOnePng();
std::vector<std::uint8_t> OneByOneJpeg();
std::vector<std::uint8_t> CueSheet(std::string_view title, std::string_view performer, std::string_view imageFileName, std::string_view audioFileName);
std::vector<std::uint8_t> BuildId3v23Frame(std::string_view frameId, const std::vector<std::uint8_t> &payload);
std::vector<std::uint8_t> BuildId3v23Tag(const std::vector<std::uint8_t> &frames);
bool GenerateBaseMp3(const std::filesystem::path &path);
bool GenerateCoverSample(const std::filesystem::path &samplePath);
bool GenerateCueSampleBundle(const std::filesystem::path &sampleRoot);
}
