#ifndef TAGREADER_PLATFORM_POSIXCOMPAT_HPP
#define TAGREADER_PLATFORM_POSIXCOMPAT_HPP

// POSIX fd API 兼容层。
// Windows (_WIN32) 下将源码中使用的 POSIX 文件描述符 API 映射到 CRT 等价物
// （_wopen/_read/_write/_lseeki64/_fstat64/_commit/_locking 等），并补齐
// ssize_t/off_t 语义（64 位）、S_ISREG、O_CLOEXEC/O_NOFOLLOW、flock 等。
// 其它平台：仅提供 tagreader_stat_t 别名，其余直接使用系统 POSIX API。
//
// 使用约定：
// - 所有调用方统一 include 本头（替代裸 include <unistd.h>/<sys/stat.h>）。
// - struct stat 声明统一写为 tagreader_stat_t（64 位 st_size，Linux 上等价于 struct stat）。
// - 文件路径参数：Windows 下 open/stat/unlink 接受 const wchar_t*（std::filesystem::path::c_str()）。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cerrno>

#if defined(_WIN32)

// 统一关闭 MSVC 的 POSIX 名宏替换（io.h 在未定义该宏时会把 close/read 等
// 替换为 _close/_read，破坏本兼容层及调用方的 POSIX 名函数定义）。
// 由此 MSVC 不再提供 read/write 别名，统一由本兼容层实现。
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_IRWXU
#define S_IRWXU (_S_IREAD | _S_IWRITE)
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#endif

using ssize_t = std::intptr_t;
using tagreader_stat_t = struct _stat64;

inline int open(const wchar_t *path, int flags)
{
    return _wopen(path, flags | _O_BINARY);
}

inline int open(const wchar_t *path, int flags, int mode)
{
    return _wopen(path, flags | _O_BINARY, mode);
}

inline int close(int fd)
{
    return _close(fd);
}

// read/write 不在此定义：MSVC CRT 始终提供 POSIX 兼容声明
// （int read(int, void*, unsigned int) / int write(int, const void*, unsigned int)），
// 重定义会与调用点产生重载歧义。

// pread 语义：从指定 offset 读取，不改变 fd 的文件位置。
inline ssize_t pread(int fd, void *buffer, std::size_t count, std::int64_t offset)
{
    const auto savedOffset = _lseeki64(fd, 0, SEEK_CUR);
    if (savedOffset == -1)
    {
        return -1;
    }
    if (_lseeki64(fd, offset, SEEK_SET) == -1)
    {
        return -1;
    }
    const auto bytesRead = _read(fd, buffer, static_cast<unsigned int>(count));
    _lseeki64(fd, savedOffset, SEEK_SET);
    return bytesRead;
}

inline int fstat(int fd, tagreader_stat_t *buffer)
{
    return _fstat64(fd, buffer);
}

inline int stat(const wchar_t *path, tagreader_stat_t *buffer)
{
    return _wstat64(path, buffer);
}

inline int unlink(const wchar_t *path)
{
    return _wunlink(path);
}

inline int getpid()
{
    return _getpid();
}

inline int fsync(int fd)
{
    return _commit(fd);
}

inline int flock(int fd, int operation)
{
    const int mode = ((operation & LOCK_NB) != 0) ? _LK_NBLCK : _LK_LOCK;
    return _locking(fd, mode, 1);
}

// 硬链接发布（原子“不存在则创建”）。成功返回 0；目标已存在时映射 errno=EEXIST。
inline int link(const wchar_t *existingPath, const wchar_t *newPath)
{
    if (CreateHardLinkW(newPath, existingPath, nullptr) != 0)
    {
        return 0;
    }
    const DWORD error = GetLastError();
    errno = (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) ? EEXIST : EINVAL;
    return -1;
}

#else

#include <fcntl.h>
// flock()/LOCK_SH/LOCK_EX/LOCK_NB 的声明所在（glibc/macOS/BSD 通用）；
// 勿移除——<fcntl.h> 只提供 struct flock 类型，缺此头会让 ::flock(...)
// 被解析为函数式类型转换而编译失败。
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using tagreader_stat_t = struct stat;

#endif

#endif // TAGREADER_PLATFORM_POSIXCOMPAT_HPP
