#ifndef TAGREADER_COVER_COVERCACHE_HPP
#define TAGREADER_COVER_COVERCACHE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace tagreader_cover
{
std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size);
}

#endif
