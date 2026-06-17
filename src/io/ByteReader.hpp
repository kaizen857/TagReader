#ifndef TAGREADER_IO_BYTEREADER_HPP
#define TAGREADER_IO_BYTEREADER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tagreader_io
{
class FileInput
{
public:
    FileInput() noexcept = default;
    explicit FileInput(int fd) noexcept;
    ~FileInput();

    FileInput(const FileInput &) = delete;
    FileInput &operator=(const FileInput &) = delete;
    FileInput(FileInput &&other) noexcept;
    FileInput &operator=(FileInput &&other) noexcept;

    int get() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;
    bool is_open() const noexcept;
    void clear() noexcept;

private:
    int fd_{-1};
};

class ByteCursor
{
public:
    ByteCursor(const uint8_t *data, std::size_t size);

    std::size_t remaining() const;
    std::optional<std::uint32_t> readU32Be();
    std::optional<std::span<const uint8_t>> readBytes(std::size_t n);
    bool skip(std::size_t n);

private:
    const uint8_t *data_{};
    std::size_t size_{};
    std::size_t offset_{};
};

uint32_t ReadBE32(const uint8_t *data);
uint32_t ReadBE24(const uint8_t *data);
uint32_t ReadBE16(const uint8_t *data);
uint32_t ReadLE32(const uint8_t *data);
uint32_t ReadSyncSafe32(const uint8_t *data);
bool IsValidSyncSafe32(const uint8_t *data);
bool TryAddUintmax(std::uintmax_t base, std::uintmax_t delta, std::uintmax_t &result);
bool TryAddSize(std::size_t base, std::size_t delta, std::size_t &result);
std::vector<uint8_t> ReadRange(FileInput &input, std::uintmax_t offset, std::size_t size, std::size_t maxSize);
std::vector<uint8_t> ReadRange(FileInput &input, std::uintmax_t offset, std::size_t size);
}

#endif
