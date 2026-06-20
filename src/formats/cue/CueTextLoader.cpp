#include "formats/cue/CueTextLoader.hpp"

#include "io/ByteReader.hpp"
#include "text/TextCodec.hpp"

#include <filesystem>
#include <vector>
#include <string_view>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tagreader_cue
{
namespace
{
std::optional<std::string> DecodeBytesToUtf8(const std::vector<std::uint8_t> &bytes)
{
    const auto raw = std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const tagreader_core::DecodedField decoded = tagreader_text::DecodeRawText(raw);
    if (!decoded.success)
    {
        return std::nullopt;
    }
    return decoded.value;
}
}

std::optional<std::string> LoadCueTextUtf8(const std::filesystem::path &cuePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(cuePath, ec) || ec)
    {
        return std::nullopt;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(cuePath, ec) || ec)
    {
        return std::nullopt;
    }

#if defined(__unix__) || defined(__APPLE__)
    struct stat statBuffer
    {
    };
    if (::stat(cuePath.c_str(), &statBuffer) != 0 || !S_ISREG(statBuffer.st_mode) || statBuffer.st_size < 0)
    {
        return std::nullopt;
    }
    if (static_cast<std::uintmax_t>(statBuffer.st_size) > kMaxCueTextBytes)
    {
        return std::nullopt;
    }

    int openFlags = O_RDONLY;
#if defined(O_CLOEXEC)
    openFlags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    openFlags |= O_NOFOLLOW;
#endif
    const int fd = ::open(cuePath.c_str(), openFlags);
    if (fd < 0)
    {
        return std::nullopt;
    }

    tagreader_io::FileInput input(fd);
    const std::vector<std::uint8_t> bytes = tagreader_io::ReadRange(input, 0, static_cast<std::size_t>(statBuffer.st_size), static_cast<std::size_t>(kMaxCueTextBytes));
    if (bytes.empty() && statBuffer.st_size != 0)
    {
        return std::nullopt;
    }
    return DecodeBytesToUtf8(bytes);
#else
    (void)cuePath;
    return std::nullopt;
#endif
}
}
