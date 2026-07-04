#include "cover/CoverCache.hpp"

#include "common/ParseHelpers.hpp"
#include "cover/CoverDecoder.hpp"
#include "profiling/Profiling.hpp"
#include "TagReaderInternal.hpp"
#include "TagReader.hpp"
#include "core/ReadContext.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tagreader_cover
{
namespace
{
using tagreader_common::ToLower;

constexpr tagreader_internal::CoverDecodeLimits kCoverDecodeLimits{};
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;
constexpr std::array<uint8_t, 8> kPngSignature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

std::string HexEncode(const std::array<uint8_t, 32> &digest)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string hex(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        hex[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }

    return hex;
}

std::string HashEmbeddedImageBytes(const uint8_t *data, std::size_t size)
{
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    if (!sha)
    {
        throw std::runtime_error("cover cache failed to allocate SHA-256 context");
    }
    if (av_sha_init(sha.get(), 256) != 0)
    {
        throw std::runtime_error("cover cache failed to initialize SHA-256 context");
    }

    av_sha_update(sha.get(), data, static_cast<unsigned int>(size));

    std::array<uint8_t, 32> digest{};
    av_sha_final(sha.get(), digest.data());

    return HexEncode(digest);
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

bool IsReusableCoverCacheFile(const std::filesystem::path &path, const std::vector<uint8_t> &expectedBytes)
{
    std::error_code statusEc;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, statusEc);
    if (statusEc)
    {
        if (statusEc == std::errc::no_such_file_or_directory)
        {
            return false;
        }
        throw std::runtime_error("cover cache failed to query file " + path.string() + ": " + statusEc.message());
    }
    if (!std::filesystem::exists(status))
    {
        return false;
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
    if (statBuffer.st_size <= 0)
    {
        ThrowCoverCacheValidationError(path, "invalid file size");
    }
    if (static_cast<std::uintmax_t>(statBuffer.st_size) > kCoverDecodeLimits.maxOutputBytes)
    {
        ThrowCoverCacheValidationError(path, "file is oversized");
    }

    if (static_cast<std::uintmax_t>(statBuffer.st_size) != expectedBytes.size())
    {
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        return false;
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
            std::error_code removeEc;
            std::filesystem::remove(path, removeEc);
            return false;
        }
        if (result == 0)
        {
            std::error_code removeEc;
            std::filesystem::remove(path, removeEc);
            return false;
        }
        readBytes += static_cast<std::size_t>(result);
    }

    if (existingBytes != expectedBytes)
    {
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        return false;
    }

    return true;
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
    if (!std::filesystem::is_directory(finalPath.parent_path(), ec))
    {
        if (ec)
        {
            throw std::runtime_error("cover cache failed to query directory " + finalPath.parent_path().string() + ": " + ec.message());
        }
        throw std::runtime_error("cover cache path parent is not a directory: " + finalPath.parent_path().string());
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
    TAGREADER_PROFILE_FUNCTION();

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
    if (!std::filesystem::is_directory(coverPath.parent_path(), ec))
    {
        if (ec)
        {
            throw std::runtime_error("cover cache failed to query shard directory " + coverPath.parent_path().string() + ": " + ec.message());
        }
        throw std::runtime_error("cover cache shard path is not a directory: " + coverPath.parent_path().string());
    }
    if (ToLower(coverPath.extension().string()) != ".png")
    {
        throw std::runtime_error("cover export path must use .png extension: " + coverPath.string());
    }

    static std::array<std::mutex, 4096> coverMutexes;
    const auto coverHash = coverPath.filename().string();
    const auto mutexIndex = std::hash<std::string>{}(coverHash) % 4096;

    std::lock_guard<std::mutex> lock(coverMutexes[mutexIndex]);

    if (std::filesystem::exists(coverPath))
    {
        return coverPath;
    }

    // 优化：直接解码到 RGB24，跳过中间 PNG 往返
    DecodedImage decoded = DecodeImageToRgb24Direct(data, size);
    if (decoded.frame == nullptr)
    {
        // Fallback 到旧路径
        decoded = DecodeImage(data, size);
        if (decoded.frame == nullptr)
        {
            return {};
        }
    }

    PngEncodeOptions encOpts;
    encOpts.compressionLevel = 6;
    std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
    FreeDecodedImage(decoded);

    if (png.empty())
    {
        return {};
    }

    const std::filesystem::path lockPath = coverPath.string() + ".lock";
    FileDescriptor lockFd(::open(lockPath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600));
    if (lockFd.get() < 0)
    {
        throw std::runtime_error("cover cache failed to create lock file " + lockPath.string() + ": " + std::strerror(errno));
    }

    if (::flock(lockFd.get(), LOCK_EX) != 0)
    {
        throw std::runtime_error("cover cache failed to acquire lock " + lockPath.string() + ": " + std::strerror(errno));
    }

    if (IsReusableCoverCacheFile(coverPath, png))
    {
        return coverPath;
    }

    if (!AtomicWriteFileIfAbsent(coverPath, png.data(), png.size()))
    {
        return {};
    }
    ValidateExistingCoverCacheFile(coverPath, png);
    return coverPath;
}

std::filesystem::path BuildThumbnailCachePath(const std::filesystem::path &coverExportDir, std::string_view hex)
{
    if (coverExportDir.empty() || hex.size() < 3)
    {
        return {};
    }

    return coverExportDir / "thumbnails" / std::string(hex.substr(0, 2)) / (std::string(hex.substr(2)) + ".png");
}

CoverPaths WriteCoverWithThumbnail(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size, const CoverProcessingOptions &options)
{
    TAGREADER_PROFILE_FUNCTION();

    if (coverExportDir.empty() || data == nullptr || size == 0 || size > kMaxCoverInputBytes)
    {
        return {};
    }

    const std::string contentHash = HashEmbeddedImageBytes(data, size);
    const std::filesystem::path fullPath = BuildCoverCachePath(coverExportDir, contentHash);
    const std::filesystem::path thumbPath = options.generateThumbnail ? BuildThumbnailCachePath(coverExportDir, contentHash) : std::filesystem::path{};

    std::error_code ec;
    if (std::filesystem::exists(fullPath, ec) && (!options.generateThumbnail || std::filesystem::exists(thumbPath, ec)))
    {
        return {fullPath, thumbPath};
    }

    // 方案 B：解码和缩放移出锁外，减小临界区
    // 优化：优先尝试直接解码到 RGB24，跳过中间 PNG 编码/解码
    DecodedImage decoded = DecodeImageToRgb24Direct(data, size);
    if (decoded.frame == nullptr)
    {
        // Fallback 到旧路径（中间 PNG）
        decoded = DecodeImage(data, size);
        if (decoded.frame == nullptr)
        {
            return {};
        }
    }

    DecodedImage thumbnail;
    if (options.generateThumbnail)
    {
        ThumbnailOptions thumbOpts;
        thumbOpts.maxWidth = options.thumbnailSize.width;
        thumbOpts.maxHeight = options.thumbnailSize.height;
        thumbOpts.maintainAspectRatio = options.thumbnailSize.maintainAspectRatio;
        thumbOpts.scalingQuality = static_cast<int>(options.scalingQuality);

        thumbnail = GenerateThumbnail(decoded, thumbOpts);
        if (thumbnail.frame == nullptr)
        {
            FreeDecodedImage(decoded);
            return {};
        }
    }

    // 方案 A：扩大分片锁到 4096（从 256）
    static std::array<std::mutex, 4096> coverMutexes;
    const auto mutexIndex = std::hash<std::string>{}(contentHash) % 4096;
    std::lock_guard<std::mutex> lock(coverMutexes[mutexIndex]);

    // 双重检查（在锁内）
    if (std::filesystem::exists(fullPath, ec) && (!options.generateThumbnail || std::filesystem::exists(thumbPath, ec)))
    {
        FreeDecodedImage(decoded);
        if (thumbnail.frame != nullptr)
        {
            FreeDecodedImage(thumbnail);
        }
        return {fullPath, thumbPath};
    }

    std::future<bool> fullFuture = std::async(std::launch::async, [&]() {
        if (std::filesystem::exists(fullPath))
        {
            return true;
        }

        PngEncodeOptions encOpts;
        encOpts.compressionLevel = 6;
        std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
        if (png.empty())
        {
            return false;
        }

        return AtomicWriteFileIfAbsent(fullPath, png.data(), png.size());
    });

    std::future<bool> thumbFuture;
    if (options.generateThumbnail && thumbnail.frame != nullptr)
    {
        thumbFuture = std::async(std::launch::async, [&]() {
            if (std::filesystem::exists(thumbPath))
            {
                return true;
            }

            PngEncodeOptions encOpts;
            encOpts.compressionLevel = static_cast<int>(options.pngCompression);
            std::vector<uint8_t> png = EncodePngWithOptions(thumbnail, encOpts);
            if (png.empty())
            {
                return false;
            }

            return AtomicWriteFileIfAbsent(thumbPath, png.data(), png.size());
        });
    }

    const bool fullSuccess = fullFuture.get();
    const bool thumbSuccess = !options.generateThumbnail || (thumbFuture.valid() && thumbFuture.get());

    FreeDecodedImage(decoded);
    if (options.generateThumbnail)
    {
        FreeDecodedImage(thumbnail);
    }

    if (!fullSuccess || !thumbSuccess)
    {
        return {};
    }

    return {fullPath, thumbPath};
}

CoverPaths ExportCoverFromContext(const tagreader_core::ReadContext &context, const uint8_t *data, std::size_t size)
{
    if (context.coverOptions != nullptr && context.coverOptions->generateThumbnail)
    {
        return WriteCoverWithThumbnail(context.coverExportDir, data, size, *context.coverOptions);
    }
    else
    {
        return {WriteCoverAsPng(context.coverExportDir, data, size), {}};
    }
}
}
