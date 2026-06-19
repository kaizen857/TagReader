#ifndef TAGREADER_FORMATS_COMMON_BOUNDEDREADER_HPP
#define TAGREADER_FORMATS_COMMON_BOUNDEDREADER_HPP

#include "core/ReadContext.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tagreader_core::formats
{
constexpr std::uint64_t kDefaultMaxBoundedReadBytes = 64ULL * 1024ULL * 1024ULL;

struct BoundedRange
{
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t end{};
};

struct BoundedChunkRange
{
    std::uint64_t payloadOffset{};
    std::uint64_t payloadSize{};
    std::uint64_t payloadEnd{};
    std::uint64_t paddedEnd{};
};

std::optional<BoundedRange> MakeBoundedRange(std::uint64_t offset, std::uint64_t size, std::uint64_t parentEnd);
std::optional<BoundedChunkRange> MakeBoundedChunkRange(std::uint64_t payloadOffset,
                                                       std::uint64_t payloadSize,
                                                       std::uint64_t parentEnd,
                                                       std::uint64_t paddingAlignment = 2);

std::vector<std::uint8_t> ReadRangeAt(ReadContext &context,
                                      std::uint64_t offset,
                                      std::uint64_t size,
                                      std::uint64_t parentEnd);
std::vector<std::uint8_t> ReadRangeAt(ReadContext &context,
                                      std::uint64_t offset,
                                      std::uint64_t size,
                                      std::uint64_t parentEnd,
                                      std::uint64_t maxSize);

std::optional<std::uint16_t> ReadU16Le(std::span<const std::uint8_t> bytes);
std::optional<std::uint16_t> ReadU16Be(std::span<const std::uint8_t> bytes);
std::optional<std::uint32_t> ReadU24Le(std::span<const std::uint8_t> bytes);
std::optional<std::uint32_t> ReadU24Be(std::span<const std::uint8_t> bytes);
std::optional<std::uint32_t> ReadU32Le(std::span<const std::uint8_t> bytes);
std::optional<std::uint32_t> ReadU32Be(std::span<const std::uint8_t> bytes);
std::optional<std::uint64_t> ReadU64Le(std::span<const std::uint8_t> bytes);
std::optional<std::uint64_t> ReadU64Be(std::span<const std::uint8_t> bytes);

class BoundedCursor
{
public:
    BoundedCursor(const std::uint8_t *data, std::size_t size) noexcept;
    explicit BoundedCursor(std::span<const std::uint8_t> bytes) noexcept;

    std::size_t offset() const noexcept;
    std::size_t remaining() const noexcept;
    bool empty() const noexcept;

    std::optional<std::uint8_t> readU8();
    std::optional<std::uint16_t> readU16Le();
    std::optional<std::uint16_t> readU16Be();
    std::optional<std::uint32_t> readU24Le();
    std::optional<std::uint32_t> readU24Be();
    std::optional<std::uint32_t> readU32Le();
    std::optional<std::uint32_t> readU32Be();
    std::optional<std::uint64_t> readU64Le();
    std::optional<std::uint64_t> readU64Be();
    std::optional<std::span<const std::uint8_t>> readBytes(std::size_t size);
    bool skip(std::size_t size);

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};
}

#endif
