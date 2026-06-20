#ifndef TAGREADER_FORMATS_CUE_CUETEXTLOADER_HPP
#define TAGREADER_FORMATS_CUE_CUETEXTLOADER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tagreader_cue
{
constexpr std::uintmax_t kMaxCueTextBytes = 4ULL * 1024ULL * 1024ULL;

std::optional<std::string> LoadCueTextUtf8(const std::filesystem::path &cuePath);
}

#endif
