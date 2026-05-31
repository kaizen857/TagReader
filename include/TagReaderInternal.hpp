#ifndef TAGREADER_INTERNAL_HPP
#define TAGREADER_INTERNAL_HPP

#include <cstddef>
#include <cstdint>

namespace tagreader_internal
{
struct CoverDecodeLimits
{
    std::size_t maxInputBytes{64 * 1024 * 1024};
    int maxWidth{8192};
    int maxHeight{8192};
    std::int64_t maxPixels{32LL * 1024 * 1024};
    std::size_t maxOutputBytes{64 * 1024 * 1024};
};
} // namespace tagreader_internal

#endif
