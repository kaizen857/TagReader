#include "io/ByteReader.hpp"

#include <limits>

namespace tagreader_io
{
namespace
{
constexpr std::size_t kMaxGenericReadBytes = 64z * 1024 * 1024;
}

ByteCursor::ByteCursor(const uint8_t *data, std::size_t size) : data_(data), size_(size)
{
}

std::size_t ByteCursor::remaining() const
{
    return offset_ <= size_ ? size_ - offset_ : 0;
}

std::optional<std::uint32_t> ByteCursor::readU32Be()
{
    if (remaining() < 4)
    {
        return std::nullopt;
    }

    const std::uint32_t value = ReadBE32(data_ + offset_);
    offset_ += 4;
    return value;
}

std::optional<std::span<const uint8_t>> ByteCursor::readBytes(std::size_t n)
{
    if (n > remaining())
    {
        return std::nullopt;
    }

    const auto bytes = std::span<const uint8_t>(data_ + offset_, n);
    offset_ += n;
    return bytes;
}

bool ByteCursor::skip(std::size_t n)
{
    if (n > remaining())
    {
        return false;
    }

    offset_ += n;
    return true;
}

uint32_t ReadBE32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t ReadBE24(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) | static_cast<uint32_t>(data[2]);
}

uint32_t ReadBE16(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 8) | static_cast<uint32_t>(data[1]);
}

uint32_t ReadLE32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t ReadSyncSafe32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 21) | (static_cast<uint32_t>(data[1]) << 14) | (static_cast<uint32_t>(data[2]) << 7) | static_cast<uint32_t>(data[3]);
}

bool IsValidSyncSafe32(const uint8_t *data)
{
    return (data[0] & 0x80) == 0 && (data[1] & 0x80) == 0 && (data[2] & 0x80) == 0 && (data[3] & 0x80) == 0;
}

bool TryAddUintmax(std::uintmax_t base, std::uintmax_t delta, std::uintmax_t &result)
{
    if (base > std::numeric_limits<std::uintmax_t>::max() - delta)
    {
        return false;
    }
    result = base + delta;
    return true;
}

bool TryAddSize(std::size_t base, std::size_t delta, std::size_t &result)
{
    if (base > std::numeric_limits<std::size_t>::max() - delta)
    {
        return false;
    }
    result = base + delta;
    return true;
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size, std::size_t maxSize)
{
    if (size > maxSize)
    {
        input.clear();
        return {};
    }
    if (offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
    {
        input.clear();
        return {};
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        input.clear();
        return {};
    }
    if (offset > std::numeric_limits<std::uintmax_t>::max() - static_cast<std::uintmax_t>(size))
    {
        input.clear();
        return {};
    }

    std::vector<uint8_t> buffer(size);
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
    {
        input.clear();
        return {};
    }

    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() != static_cast<std::streamsize>(buffer.size()))
    {
        input.clear();
        return {};
    }

    return buffer;
}

std::vector<uint8_t> ReadRange(std::ifstream &input, std::uintmax_t offset, std::size_t size)
{
    return ReadRange(input, offset, size, kMaxGenericReadBytes);
}
}
