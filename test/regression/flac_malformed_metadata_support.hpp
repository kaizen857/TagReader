#pragma once

#include <filesystem>
#include <string_view>

namespace flac_malformed_test
{
bool RunLaterValidVorbisBlockSurvives(const std::filesystem::path &root);
bool RunFlacPictureCoverCacheFailurePropagates(const std::filesystem::path &root);
std::filesystem::path MakeCaseRoot(std::string_view caseName);
bool PrepareCaseRoot(const std::filesystem::path &root);
}
