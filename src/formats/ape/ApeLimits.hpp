#ifndef TAGREADER_FORMATS_APE_APELIMITS_HPP
#define TAGREADER_FORMATS_APE_APELIMITS_HPP

#include <cstddef>

namespace tagreader_ape
{
constexpr std::size_t kMaxApeTagBytes = 16z * 1024 * 1024;   // 16 MiB
constexpr std::size_t kMaxApeItems = 4096;                    // maximum item count
constexpr std::size_t kMaxApeItemValueBytes = 1z * 1024 * 1024; // 1 MiB per item value
}

#endif
