#include "cover/CoverCache.hpp"

#include "common/ParseHelpers.hpp"
#include "core/CoverBudget.hpp"
#include "cover/CoverDecoder.hpp"
#include "platform/PosixCompat.hpp"
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
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

// POSIX fd API（open/close/read/write/fstat/unlink/getpid/fsync/flock 及 LOCK_*/O_CLOEXEC 宏）
// 由 platform/PosixCompat.hpp 提供（Windows 下映射到 CRT 等价物）。

namespace tagreader_cover
{
namespace
{
using tagreader_common::ToLower;

constexpr tagreader_internal::CoverDecodeLimits kCoverDecodeLimits{};
constexpr std::size_t kMaxCoverInputBytes = 64z * 1024 * 1024;
constexpr std::array<uint8_t, 8> kPngSignature{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

[[nodiscard]] std::string Utf8Text(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string{reinterpret_cast<const char *>(utf8.data()), utf8.size()};
}

[[noreturn]] void ThrowTypedCoverError(CoverErrorCode code, const std::string &message, const std::filesystem::path &path = {})
{
    throw CoverProcessingError{code, message, path};
}

// Serializes writers of the same content-addressed cover within one process.
std::unique_lock<std::mutex> LockCoverCacheShard(std::string_view contentHash)
{
    static std::array<std::mutex, 4096> coverMutexes;
    const auto mutexIndex = std::hash<std::string>{}(std::string(contentHash)) % 4096;
    return std::unique_lock<std::mutex>(coverMutexes[mutexIndex]);
}

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
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to allocate SHA-256 context");
    }
    if (av_sha_init(sha.get(), 256) != 0)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to initialize SHA-256 context");
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

    return coverExportDir / "artwork" / std::string(hex.substr(0, 2)) / (std::string(hex.substr(2)) + ".png");
}

[[noreturn]] void ThrowCoverCacheValidationError(const std::filesystem::path &path, const std::string &reason)
{
    ThrowTypedCoverError(CoverErrorCode::CacheReadFailed, "invalid cover cache file " + Utf8Text(path) + ": " + reason, path);
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
    const auto now = static_cast<long long>(
        std::filesystem::file_time_type::clock::now().time_since_epoch().count());
    const auto seq = tempCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string name = Utf8Text(finalPath.filename()) + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(now) + "." + std::to_string(seq);
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
#if defined(_WIN32)
    // Windows 无法以只读方式打开目录句柄（_wopen 返回 Permission denied），
    // 且 NTFS 目录项耐久性由文件系统保证，文件本身已通过 _commit 落盘，
    // 目录 fsync 在此平台上无对应语义，直接跳过。
    (void)directory;
#else
    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    FileDescriptor fd(::open(directory.c_str(), flags));
    if (fd.get() < 0)
    {
        ThrowTypedCoverError(CoverErrorCode::PublicationFailed, "cover cache failed to open directory for fsync: " + Utf8Text(directory) + ": " + std::strerror(errno), directory);
    }
    if (::fsync(fd.get()) != 0)
    {
        ThrowTypedCoverError(CoverErrorCode::PublicationFailed, "cover cache failed to fsync directory: " + Utf8Text(directory) + ": " + std::strerror(errno), directory);
    }
#endif
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
            ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to write cover file: " + Utf8Text(path) + ": " + std::strerror(errno), path);
        }
        if (result == 0)
        {
            ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to write complete cover file: " + Utf8Text(path), path);
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

    tagreader_stat_t statBuffer
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
        ThrowTypedCoverError(CoverErrorCode::CacheReadFailed, "cover cache failed to query file " + Utf8Text(path) + ": " + statusEc.message(), path);
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
        ThrowTypedCoverError(CoverErrorCode::CacheReadFailed, "cover cache failed to open file for validation: " + Utf8Text(path) + ": " + std::strerror(errno), path);
    }

    tagreader_stat_t statBuffer
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
    TAGREADER_PROFILE_SCOPE_COLOR("AtomicWriteFileIfAbsent", TAGREADER_COLOR_IO);
    TAGREADER_PROFILE_VALUE(size);
    
    if (data == nullptr || size == 0)
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to create directory " + Utf8Text(finalPath.parent_path()) + ": " + ec.message(), finalPath.parent_path());
    }
    if (!std::filesystem::is_directory(finalPath.parent_path(), ec))
    {
        if (ec)
        {
            ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to query directory " + Utf8Text(finalPath.parent_path()) + ": " + ec.message(), finalPath.parent_path());
        }
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache path parent is not a directory: " + Utf8Text(finalPath.parent_path()), finalPath.parent_path());
    }

    if (std::filesystem::exists(finalPath, ec))
    {
        return true;
    }
    if (ec)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to query file " + Utf8Text(finalPath) + ": " + ec.message(), finalPath);
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
            ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to create temporary cover file: " + Utf8Text(tempPath) + ": " + std::strerror(errno), tempPath);
        }

        try
        {
            {
                TAGREADER_PROFILE_SCOPE_COLOR("WriteAll", TAGREADER_COLOR_IO);
                WriteAll(fd.get(), data, size, tempPath);
            }
            {
                TAGREADER_PROFILE_SCOPE_COLOR("fsync", TAGREADER_COLOR_IO);
                if (::fsync(fd.get()) != 0)
                {
                    ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to fsync cover file: " + Utf8Text(tempPath) + ": " + std::strerror(errno), tempPath);
                }
            }
            if (::close(fd.release()) != 0)
            {
                ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to close cover file: " + Utf8Text(tempPath) + ": " + std::strerror(errno), tempPath);
            }
        }
        catch (...)
        {
            std::error_code removeEc;
            std::filesystem::remove(tempPath, removeEc);
            throw;
        }

        PublishResult publishResult;
        {
            TAGREADER_PROFILE_SCOPE_COLOR("PublishFileIfAbsent", TAGREADER_COLOR_IO);
            publishResult = PublishFileIfAbsent(tempPath, finalPath);
        }
        if (publishResult == PublishResult::Published || publishResult == PublishResult::AlreadyExists)
        {
            return true;
        }
        ThrowTypedCoverError(CoverErrorCode::PublicationFailed, "cover export failed to publish cover file: " + Utf8Text(finalPath) + ": " + std::strerror(errno), finalPath);
    }

    ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export failed to create unique temporary cover file for: " + Utf8Text(finalPath), finalPath);
}
}

// Debits `size` encoded source-cover bytes against the per-read budget. Returns
// false when the source-art read must be skipped without error (zero budget);
// throws SourceBudgetExceeded when the cumulative budget would be exceeded.
bool DebitCoverSourceBudget(tagreader_core::ReadContext &context, std::size_t size)
{
    const CoverProcessingOptions *options = context.coverOptions;
    if (options == nullptr || size == 0)
    {
        return true;
    }
    if (options->maxSourceCoverBytes == 0)
    {
        return false;
    }
    if (tagreader_core::ExceedsCoverSourceBudget(context.coverSourceBytesDebited, size, options->maxSourceCoverBytes))
    {
        ThrowTypedCoverError(CoverErrorCode::SourceBudgetExceeded,
                             "encoded source cover bytes exceed maxSourceCoverBytes",
                             context.filePath);
    }
    context.coverSourceBytesDebited += size;
    return true;
}

std::filesystem::path WriteCoverAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size)
{
    TAGREADER_PROFILE_FUNCTION();

    if (coverExportDir.empty() || data == nullptr || size == 0 || size > kMaxCoverInputBytes)
    {
        return {};
    }

    const std::string contentHash = HashEmbeddedImageBytes(data, size);
    const std::filesystem::path coverPath = BuildCoverCachePath(coverExportDir, contentHash);
    std::error_code ec;
    std::filesystem::create_directories(coverPath.parent_path(), ec);
    if (ec)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to create shard directory " + Utf8Text(coverPath.parent_path()) + ": " + ec.message(), coverPath.parent_path());
    }
    if (!std::filesystem::is_directory(coverPath.parent_path(), ec))
    {
        if (ec)
        {
            ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to query shard directory " + Utf8Text(coverPath.parent_path()) + ": " + ec.message(), coverPath.parent_path());
        }
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache shard path is not a directory: " + Utf8Text(coverPath.parent_path()), coverPath.parent_path());
    }
    if (ToLower(Utf8Text(coverPath.extension())) != ".png")
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover export path must use .png extension: " + Utf8Text(coverPath), coverPath);
    }

    std::unique_lock<std::mutex> lock = LockCoverCacheShard(contentHash);

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
    std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
    FreeDecodedImage(decoded);

    if (png.empty())
    {
        return {};
    }

    std::filesystem::path lockPath = coverPath;
    lockPath += ".lock";
    FileDescriptor lockFd(::open(lockPath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600));
    if (lockFd.get() < 0)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to create lock file " + Utf8Text(lockPath) + ": " + std::strerror(errno), lockPath);
    }

    if (::flock(lockFd.get(), LOCK_EX) != 0)
    {
        ThrowTypedCoverError(CoverErrorCode::CacheWriteFailed, "cover cache failed to acquire lock " + Utf8Text(lockPath) + ": " + std::strerror(errno), lockPath);
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
    std::unique_lock<std::mutex> lock = LockCoverCacheShard(contentHash);

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

    // 顺序编码：先 full-size，后 thumbnail
    // 避免嵌套并发（scanner 已有 32-way worker pool）
    bool fullSuccess = false;
    if (!std::filesystem::exists(fullPath))
    {
        PngEncodeOptions encOpts;
        std::vector<uint8_t> png = EncodePngWithOptions(decoded, encOpts);
        if (!png.empty())
        {
            fullSuccess = AtomicWriteFileIfAbsent(fullPath, png.data(), png.size());
        }
    }
    else
    {
        fullSuccess = true;
    }

    bool thumbSuccess = true;
    if (options.generateThumbnail && thumbnail.frame != nullptr)
    {
        if (!std::filesystem::exists(thumbPath))
        {
            PngEncodeOptions encOpts;
            encOpts.compressionLevel = static_cast<int>(options.pngCompression);
            std::vector<uint8_t> png = EncodePngWithOptions(thumbnail, encOpts);
            if (!png.empty())
            {
                thumbSuccess = AtomicWriteFileIfAbsent(thumbPath, png.data(), png.size());
            }
            else
            {
                thumbSuccess = false;
            }
        }
    }

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

std::filesystem::path WriteThumbnailAsPng(const std::filesystem::path &coverExportDir, const uint8_t *data, std::size_t size, const CoverProcessingOptions &options)
{
    if (coverExportDir.empty() || data == nullptr || size == 0 || size > kMaxCoverInputBytes)
    {
        return {};
    }

    const std::string contentHash = HashEmbeddedImageBytes(data, size);
    const std::filesystem::path thumbPath = BuildThumbnailCachePath(coverExportDir, contentHash);

    std::error_code ec;
    if (std::filesystem::exists(thumbPath, ec))
    {
        return thumbPath;
    }

    DecodedImage decoded = DecodeImageToRgb24Direct(data, size);
    if (decoded.frame == nullptr)
    {
        decoded = DecodeImage(data, size);
        if (decoded.frame == nullptr)
        {
            return {};
        }
    }

    ThumbnailOptions thumbOpts;
    thumbOpts.maxWidth = options.thumbnailSize.width;
    thumbOpts.maxHeight = options.thumbnailSize.height;
    thumbOpts.maintainAspectRatio = options.thumbnailSize.maintainAspectRatio;
    thumbOpts.scalingQuality = static_cast<int>(options.scalingQuality);

    DecodedImage thumbnail = GenerateThumbnail(decoded, thumbOpts);
    FreeDecodedImage(decoded);
    if (thumbnail.frame == nullptr)
    {
        return {};
    }

    PngEncodeOptions encOpts;
    encOpts.compressionLevel = static_cast<int>(options.pngCompression);
    std::vector<uint8_t> png = EncodePngWithOptions(thumbnail, encOpts);
    FreeDecodedImage(thumbnail);
    if (png.empty())
    {
        return {};
    }

    std::unique_lock<std::mutex> lock = LockCoverCacheShard(contentHash);

    if (std::filesystem::exists(thumbPath))
    {
        return thumbPath;
    }

    if (IsReusableCoverCacheFile(thumbPath, png))
    {
        return thumbPath;
    }

    if (!AtomicWriteFileIfAbsent(thumbPath, png.data(), png.size()))
    {
        return {};
    }
    ValidateExistingCoverCacheFile(thumbPath, png);
    return thumbPath;
}

CoverPaths ExportCoverFromContext(tagreader_core::ReadContext &context, const uint8_t *data, std::size_t size)
{
    if (context.coverOptions == nullptr)
    {
        return {WriteCoverAsPng(context.coverExportDir, data, size), {}};
    }

    const CoverProcessingOptions &options = *context.coverOptions;
    if (options.mode == CoverProcessingOptions::CoverProcessingMode::Disabled)
    {
        return {};
    }

    if (!DebitCoverSourceBudget(context, size))
    {
        return {};
    }

    switch (options.mode)
    {
    case CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly:
        return {{}, WriteThumbnailAsPng(context.coverExportDir, data, size, options)};
    case CoverProcessingOptions::CoverProcessingMode::FullOnly:
        return {WriteCoverAsPng(context.coverExportDir, data, size), {}};
    case CoverProcessingOptions::CoverProcessingMode::FullAndThumbnail:
        return WriteCoverWithThumbnail(context.coverExportDir, data, size, options);
    case CoverProcessingOptions::CoverProcessingMode::Disabled:
        break;
    }
    return {};
}
}
