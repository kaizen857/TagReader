#ifndef TAGREADER_INTERNAL_HPP
#define TAGREADER_INTERNAL_HPP

#include <cstdint>
#include <cstddef>
#include <string>

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

struct Mp4AtomHeader
{
    std::uintmax_t offset{};
    std::uintmax_t headerSize{};
    std::uintmax_t payloadOffset{};
    std::uintmax_t atomEnd{};
    std::uint64_t atomSize{};
    std::string atomType;
};

enum class Mp4PathState
{
    Root,
    Moov,
    Udta,
    Meta,
    Ilst,
};

struct PendingMp4AtomRange
{
    std::uintmax_t offset{};
    std::uintmax_t limit{};
    Mp4PathState state{Mp4PathState::Root};
};
} // namespace tagreader_internal

#endif
