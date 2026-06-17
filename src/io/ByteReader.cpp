#include "io/ByteReader.hpp"

#include <cerrno>
#include <limits>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace tagreader_io
{
namespace
{
constexpr std::size_t kMaxGenericReadBytes = 64z * 1024 * 1024;
}

FileInput::FileInput(int fd) noexcept : fd_(fd)
{
}

FileInput::~FileInput()
{
    reset();
}

FileInput::FileInput(FileInput &&other) noexcept : fd_(other.release())
{
}

FileInput &FileInput::operator=(FileInput &&other) noexcept
{
    if (this != &other)
    {
        reset(other.release());
    }
    return *this;
}

int FileInput::get() const noexcept
{
    return fd_;
}

int FileInput::release() noexcept
{
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void FileInput::reset(int fd) noexcept
{
#if defined(__unix__) || defined(__APPLE__)
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
#endif
    fd_ = fd;
}

bool FileInput::is_open() const noexcept
{
    return fd_ >= 0;
}

void FileInput::clear() noexcept
{
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

std::vector<uint8_t> ReadRange(FileInput &input, std::uintmax_t offset, std::size_t size, std::size_t maxSize)
{
    if (size > maxSize)
    {
        return {};
    }
    if (!input.is_open())
    {
        return {};
    }
#if defined(__unix__) || defined(__APPLE__)
    if (offset > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
    {
        return {};
    }
    if (offset > std::numeric_limits<std::uintmax_t>::max() - static_cast<std::uintmax_t>(size))
    {
        return {};
    }

    std::vector<uint8_t> buffer(size);
    std::size_t totalRead = 0;
    while (totalRead < size)
    {
        const auto currentOffset = static_cast<off_t>(offset + totalRead);
        const ssize_t bytesRead = ::pread(input.get(), buffer.data() + totalRead, size - totalRead, currentOffset);
        if (bytesRead < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return {};
        }
        if (bytesRead == 0)
        {
            return {};
        }

        totalRead += static_cast<std::size_t>(bytesRead);
    }

    return buffer;
#else
    (void)offset;
    (void)size;
    return {};
#endif
}

std::vector<uint8_t> ReadRange(FileInput &input, std::uintmax_t offset, std::size_t size)
{
    return ReadRange(input, offset, size, kMaxGenericReadBytes);
}
}
