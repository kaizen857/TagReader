#include "formats/common/BoundedReader.hpp"

#include "io/ByteReader.hpp"

#include <limits>

namespace tagreader_core::formats
{
namespace
{
std::optional<std::uint64_t> TryAddU64(std::uint64_t base, std::uint64_t delta)
{
    if (base > std::numeric_limits<std::uint64_t>::max() - delta)
    {
        return std::nullopt;
    }

    return base + delta;
}

bool FitsSize(std::uint64_t value)
{
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}
}

std::optional<BoundedRange> MakeBoundedRange(std::uint64_t offset, std::uint64_t size, std::uint64_t parentEnd)
{
    const std::optional<std::uint64_t> end = TryAddU64(offset, size);
    if (!end.has_value() || *end > parentEnd)
    {
        return std::nullopt;
    }

    return BoundedRange{offset, size, *end};
}

std::optional<BoundedChunkRange> MakeBoundedChunkRange(std::uint64_t payloadOffset,
                                                       std::uint64_t payloadSize,
                                                       std::uint64_t parentEnd,
                                                       std::uint64_t paddingAlignment)
{
    if (paddingAlignment == 0)
    {
        return std::nullopt;
    }

    const std::optional<BoundedRange> payloadRange = MakeBoundedRange(payloadOffset, payloadSize, parentEnd);
    if (!payloadRange.has_value())
    {
        return std::nullopt;
    }

    const std::uint64_t remainder = payloadSize % paddingAlignment;
    const std::uint64_t padding = remainder == 0 ? 0 : paddingAlignment - remainder;
    const std::optional<std::uint64_t> paddedEnd = TryAddU64(payloadRange->end, padding);
    if (!paddedEnd.has_value() || *paddedEnd > parentEnd)
    {
        return std::nullopt;
    }

    return BoundedChunkRange{payloadOffset, payloadSize, payloadRange->end, *paddedEnd};
}

std::vector<std::uint8_t> ReadRangeAt(ReadContext &context,
                                      std::uint64_t offset,
                                      std::uint64_t size,
                                      std::uint64_t parentEnd)
{
    return ReadRangeAt(context, offset, size, parentEnd, kDefaultMaxBoundedReadBytes);
}

std::vector<std::uint8_t> ReadRangeAt(ReadContext &context,
                                      std::uint64_t offset,
                                      std::uint64_t size,
                                      std::uint64_t parentEnd,
                                      std::uint64_t maxSize)
{
    if (!context.input.is_open() || size > maxSize || !FitsSize(size) || !FitsSize(maxSize))
    {
        return {};
    }
    if (parentEnd > context.fileSize)
    {
        return {};
    }

    const std::optional<BoundedRange> range = MakeBoundedRange(offset, size, parentEnd);
    if (!range.has_value())
    {
        return {};
    }

    context.input.clear();
    std::vector<std::uint8_t> bytes = tagreader_io::ReadRange(context.input,
                                                              static_cast<std::uintmax_t>(range->offset),
                                                              static_cast<std::size_t>(range->size),
                                                              static_cast<std::size_t>(maxSize));
    context.input.clear();
    return bytes;
}

std::optional<std::uint16_t> ReadU16Le(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 2)
    {
        return std::nullopt;
    }

    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::optional<std::uint16_t> ReadU16Be(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 2)
    {
        return std::nullopt;
    }

    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

std::optional<std::uint32_t> ReadU24Le(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 3)
    {
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16);
}

std::optional<std::uint32_t> ReadU24Be(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 3)
    {
        return std::nullopt;
    }

    return (static_cast<std::uint32_t>(bytes[0]) << 16) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           static_cast<std::uint32_t>(bytes[2]);
}

std::optional<std::uint32_t> ReadU32Le(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 4)
    {
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::optional<std::uint32_t> ReadU32Be(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 4)
    {
        return std::nullopt;
    }

    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::optional<std::uint64_t> ReadU64Le(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8)
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

std::optional<std::uint64_t> ReadU64Be(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8)
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        value = (value << 8) | bytes[i];
    }
    return value;
}

BoundedCursor::BoundedCursor(const std::uint8_t *data, std::size_t size) noexcept : bytes_(data, size)
{
}

BoundedCursor::BoundedCursor(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes)
{
}

std::size_t BoundedCursor::offset() const noexcept
{
    return offset_;
}

std::size_t BoundedCursor::remaining() const noexcept
{
    return offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
}

bool BoundedCursor::empty() const noexcept
{
    return remaining() == 0;
}

std::optional<std::uint8_t> BoundedCursor::readU8()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(1);
    if (!bytes.has_value())
    {
        return std::nullopt;
    }
    return (*bytes)[0];
}

std::optional<std::uint16_t> BoundedCursor::readU16Le()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(2);
    return bytes.has_value() ? ReadU16Le(*bytes) : std::nullopt;
}

std::optional<std::uint16_t> BoundedCursor::readU16Be()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(2);
    return bytes.has_value() ? ReadU16Be(*bytes) : std::nullopt;
}

std::optional<std::uint32_t> BoundedCursor::readU24Le()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(3);
    return bytes.has_value() ? ReadU24Le(*bytes) : std::nullopt;
}

std::optional<std::uint32_t> BoundedCursor::readU24Be()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(3);
    return bytes.has_value() ? ReadU24Be(*bytes) : std::nullopt;
}

std::optional<std::uint32_t> BoundedCursor::readU32Le()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(4);
    return bytes.has_value() ? ReadU32Le(*bytes) : std::nullopt;
}

std::optional<std::uint32_t> BoundedCursor::readU32Be()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(4);
    return bytes.has_value() ? ReadU32Be(*bytes) : std::nullopt;
}

std::optional<std::uint64_t> BoundedCursor::readU64Le()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(8);
    return bytes.has_value() ? ReadU64Le(*bytes) : std::nullopt;
}

std::optional<std::uint64_t> BoundedCursor::readU64Be()
{
    const std::optional<std::span<const std::uint8_t>> bytes = readBytes(8);
    return bytes.has_value() ? ReadU64Be(*bytes) : std::nullopt;
}

std::optional<std::span<const std::uint8_t>> BoundedCursor::readBytes(std::size_t size)
{
    if (size > remaining())
    {
        return std::nullopt;
    }

    const std::span<const std::uint8_t> bytes(bytes_.data() + offset_, size);
    offset_ += size;
    return bytes;
}

bool BoundedCursor::skip(std::size_t size)
{
    if (size > remaining())
    {
        return false;
    }

    offset_ += size;
    return true;
}
}
