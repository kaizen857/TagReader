#include "cover/CoverCache.hpp"

#include "cover/CoverDecoder.hpp"
#include "TagReaderInternal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tagreader_cover
{
namespace
{
constexpr tagreader_internal::CoverDecodeLimits kCoverDecodeLimits{};
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string HexEncode(std::uint64_t value)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (std::size_t i = 0; i < hex.size(); ++i)
    {
        const std::size_t shift = (hex.size() - 1 - i) * 4;
        hex[i] = kHex[(value >> shift) & 0x0F];
    }

    return hex;
}

std::string HashEmbeddedImageBytes(const uint8_t *data, std::size_t size)
{
    constexpr std::uint64_t kFnvOffsetA = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrimeA = 1099511628211ULL;
    constexpr std::uint64_t kFnvOffsetB = 1099511628211ULL;
    constexpr std::uint64_t kFnvPrimeB = 14695981039346656037ULL;

    std::uint64_t hashA = kFnvOffsetA;
    std::uint64_t hashB = kFnvOffsetB ^ static_cast<std::uint64_t>(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::uint64_t byte = data[i];
        hashA ^= byte;
        hashA *= kFnvPrimeA;

        hashB ^= byte + static_cast<std::uint64_t>(i & 0xFF);
        hashB *= kFnvPrimeB;
    }

    return HexEncode(hashA) + HexEncode(hashB);
}

std::filesystem::path BuildCoverCachePath(const std::filesystem::path &coverExportDir, std::string_view hex)
{
    if (coverExportDir.empty() || hex.size() < 3)
    {
        return {};
    }

    return coverExportDir / std::string(hex.substr(0, 2)) / (std::string(hex.substr(2)) + ".png");
}

[[noreturn]] void ThrowCoverCacheValidationError(const std::filesystem::path &path, const std::string &reason)
{
    throw std::runtime_error("invalid cover cache file " + path.string() + ": " + reason);
}

struct FileDescriptor
{
    int fd{-1};

    explicit FileDescriptor(int value) noexcept : fd(value)
    {
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    ~FileDescriptor()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    int get() const noexcept
    {
        return fd;
    }

    int release() noexcept
    {
        const int value = fd;
        fd = -1;
        return value;
    }
};

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path &finalPath)
{
    static std::atomic_uint64_t tempCounter{0};
    const auto now = std::filesystem::file_time_type::clock::now().time_since_epoch().count();
    const auto seq = tempCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string name = finalPath.filename().string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(now) + "." + std::to_string(seq);
    return finalPath.parent_path() / name;
}

enum class PublishResult
{
    Published,
    AlreadyExists,
    Failed,
};

void RemoveFileNoThrow(const std::filesystem::path &path) noexcept
{
    if (!path.empty())
    {
        ::unlink(path.c_str());
    }
}

void FsyncDirectory(const std::filesystem::path &directory)
{
    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    FileDescriptor fd(::open(directory.c_str(), flags));
    if (fd.get() < 0)
    {
        throw std::runtime_error("cover cache failed to open directory for fsync: " + directory.string() + ": " + std::strerror(errno));
    }
    if (::fsync(fd.get()) != 0)
    {
        throw std::runtime_error("cover cache failed to fsync directory: " + directory.string() + ": " + std::strerror(errno));
    }
}

PublishResult PublishFileIfAbsent(const std::filesystem::path &tempPath, const std::filesystem::path &finalPath)
{
    if (::link(tempPath.c_str(), finalPath.c_str()) == 0)
    {
        try
        {
            FsyncDirectory(finalPath.parent_path());
        }
        catch (...)
        {
            RemoveFileNoThrow(tempPath);
            throw;
        }
        RemoveFileNoThrow(tempPath);
        return PublishResult::Published;
    }

    const int linkErrno = errno;
    RemoveFileNoThrow(tempPath);
    if (linkErrno == EEXIST)
    {
        return PublishResult::AlreadyExists;
    }
    return PublishResult::Failed;
}

void WriteAll(int fd, const uint8_t *data, std::size_t size, const std::filesystem::path &path)
{
    std::size_t written = 0;
    while (written < size)
    {
        const std::size_t remaining = size - written;
        const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write(fd, data + written, chunk);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error("cover export failed to write cover file: " + path.string() + ": " + std::strerror(errno));
        }
        if (result == 0)
        {
            throw std::runtime_error("cover export failed to write complete cover file: " + path.string());
        }
        written += static_cast<std::size_t>(result);
    }
}

void ValidateExistingCoverCacheFile(const std::filesystem::path &path, const std::vector<uint8_t> &expectedBytes)
{
    if (expectedBytes.empty() || expectedBytes.size() > kCoverDecodeLimits.maxOutputBytes)
    {
        ThrowCoverCacheValidationError(path, "unexpected PNG byte size");
    }

    std::error_code statusEc;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, statusEc);
    if (statusEc)
    {
        ThrowCoverCacheValidationError(path, "failed to query path: " + statusEc.message());
    }
    if (std::filesystem::is_symlink(status))
    {
        ThrowCoverCacheValidationError(path, "symlink is not trusted");
    }
    if (!std::filesystem::is_regular_file(status))
    {
        ThrowCoverCacheValidationError(path, "path is not a regular file");
    }

    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    FileDescriptor fd(::open(path.c_str(), flags));
    if (fd.get() < 0)
    {
        ThrowCoverCacheValidationError(path, "failed to open for validation");
    }

    struct stat statBuffer
    {
    };
    if (::fstat(fd.get(), &statBuffer) != 0)
    {
        ThrowCoverCacheValidationError(path, "failed to stat opened file");
    }
    if (!S_ISREG(statBuffer.st_mode))
    {
        ThrowCoverCacheValidationError(path, "opened path is not a regular file");
    }
    if (statBuffer.st_size < 0)
    {
        ThrowCoverCacheValidationError(path, "invalid file size");
    }

    const auto existingSize = static_cast<std::uintmax_t>(statBuffer.st_size);
    if (existingSize > kCoverDecodeLimits.maxOutputBytes)
    {
        ThrowCoverCacheValidationError(path, "file is oversized");
    }
    if (existingSize != expectedBytes.size())
    {
        ThrowCoverCacheValidationError(path, "bytes do not match current cover");
    }

    std::vector<uint8_t> existingBytes(expectedBytes.size());
    std::size_t readBytes = 0;
    while (readBytes < existingBytes.size())
    {
        const std::size_t remaining = existingBytes.size() - readBytes;
        const std::size_t chunk = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::read(fd.get(), existingBytes.data() + readBytes, chunk);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ThrowCoverCacheValidationError(path, "failed to read for validation");
        }
        if (result == 0)
        {
            ThrowCoverCacheValidationError(path, "short read during validation");
        }
        readBytes += static_cast<std::size_t>(result);
    }

    uint8_t trailingByte{};
    const ssize_t trailingRead = ::read(fd.get(), &trailingByte, 1);
    if (trailingRead < 0)
    {
        ThrowCoverCacheValidationError(path, "failed to verify end of file");
    }
    if (trailingRead != 0)
    {
        ThrowCoverCacheValidationError(path, "file has trailing bytes");
    }
    if (existingBytes != expectedBytes)
    {
        ThrowCoverCacheValidationError(path, "bytes do not match current cover");
    }
}

bool AtomicWriteFileIfAbsent(const std::filesystem::path &finalPath, const uint8_t *data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        throw std::runtime_error("cover cache failed to create directory " + finalPath.parent_path().string() + ": " + ec.message());
    }

    if (std::filesystem::exists(finalPath, ec))
    {
        return true;
    }
    if (ec)
    {
        throw std::runtime_error("cover cache failed to query file " + finalPath.string() + ": " + ec.message());
    }

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const std::filesystem::path tempPath = MakeSiblingTempPath(finalPath);
        int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
        FileDescriptor fd(::open(tempPath.c_str(), flags, 0600));
        if (fd.get() < 0)
        {
            if (errno == EEXIST)
            {
                continue;
            }
            throw std::runtime_error("cover export failed to create temporary cover file: " + tempPath.string() + ": " + std::strerror(errno));
        }

        try
        {
            WriteAll(fd.get(), data, size, tempPath);
            if (::fsync(fd.get()) != 0)
            {
                throw std::runtime_error("cover export failed to fsync cover file: " + tempPath.string() + ": " + std::strerror(errno));
            }
            if (::close(fd.release()) != 0)
            {
                throw std::runtime_error("cover export failed to close cover file: " + tempPath.string() + ": " + std::strerror(errno));
            }
        }
        catch (...)
        {
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            throw;
        }

        const PublishResult publishResult = PublishFileIfAbsent(tempPath, finalPath);
        if (publishResult == PublishResult::Published || publishResult == PublishResult::AlreadyExists)
        {
            return true;
        }
        throw std::runtime_error("cover export failed to publish cover file: " + finalPath.string() + ": " + std::strerror(errno));
    }

    throw std::runtime_error("cover export failed to create unique temporary cover file for: " + finalPath.string());
}
}

std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size)
{
    if (coverExportDir.empty() || data == nullptr || size == 0 || size > kMaxCoverInputBytes)
    {
        return {};
    }

    const std::filesystem::path coverPath = BuildCoverCachePath(coverExportDir, HashEmbeddedImageBytes(data, size));
    std::error_code ec;
    std::filesystem::create_directories(coverPath.parent_path(), ec);
    if (ec)
    {
        throw std::runtime_error("cover cache failed to create shard directory " + coverPath.parent_path().string() + ": " + ec.message());
    }
    if (ToLower(coverPath.extension().string()) != ".png")
    {
        throw std::runtime_error("cover export path must use .png extension: " + coverPath.string());
    }

    std::vector<uint8_t> png = DecodeAndEncodeCoverPng(data, size);
    if (png.empty())
    {
        return {};
    }

    std::error_code statusEc;
    const std::filesystem::file_status status = std::filesystem::symlink_status(coverPath, statusEc);
    if (statusEc && statusEc != std::errc::no_such_file_or_directory)
    {
        throw std::runtime_error("cover cache failed to query file " + coverPath.string() + ": " + statusEc.message());
    }
    if (!statusEc && std::filesystem::exists(status))
    {
        // Hash collisions or pre-existing files are treated as untrusted until bytes match.
        ValidateExistingCoverCacheFile(coverPath, png);
        return coverPath;
    }

    if (!AtomicWriteFileIfAbsent(coverPath, png.data(), png.size()))
    {
        return {};
    }
    ValidateExistingCoverCacheFile(coverPath, png);
    return coverPath;
}
}
