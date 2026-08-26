#include "catch2_regression_support.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace tagreader_test_support
{
bool CommandSucceeds(const std::string &command)
{
    return std::system(command.c_str()) == 0;
}

bool HasFfmpeg()
{
#if defined(_WIN32)
    return CommandSucceeds("where ffmpeg >nul 2>&1");
#else
    return CommandSucceeds("command -v ffmpeg >/dev/null 2>&1");
#endif
}

bool WriteBinaryFile(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "failed to create directory for " << path.string() << ": " << ec.message() << '\n';
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        std::cerr << "failed to open file for write: " << path.string() << '\n';
        return false;
    }

    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool WriteTextFile(const std::filesystem::path &path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "failed to create directory for " << path.string() << ": " << ec.message() << '\n';
        return false;
    }

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output)
    {
        std::cerr << "failed to open text file for write: " << path.string() << '\n';
        return false;
    }

    output << text;
    return output.good();
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return input ? std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) : std::vector<std::uint8_t>{};
}

void AppendBytes(std::vector<std::uint8_t> &bytes, std::string_view text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void AppendU24BE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU32BE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU32LE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void AppendSyncSafe32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(value & 0x7F));
}

std::uint32_t ReadU24BE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::filesystem::path TemporaryArtifactRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_catch2_artifacts" / std::string(caseName);
}

bool PrepareCleanDirectory(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    ec.clear();
    std::filesystem::create_directories(path, ec);
    return !ec;
}

bool PrepareSymlink(const std::filesystem::path &target, const std::filesystem::path &link)
{
    std::error_code ec;
    std::filesystem::remove(link, ec);
    ec.clear();
    std::filesystem::create_directory_symlink(target, link, ec);
    return !ec;
}

bool SetEnvironment(std::string_view name, const std::filesystem::path &value)
{
#if defined(_WIN32)
    const std::string variable(name);
    const std::string pathValue = value.string();
    return ::_putenv_s(variable.c_str(), pathValue.c_str()) == 0;
#elif defined(__unix__) || defined(__APPLE__)
    return ::setenv(std::string(name).c_str(), value.c_str(), 1) == 0;
#else
    (void)name;
    (void)value;
    return false;
#endif
}

bool UnsetEnvironment(std::string_view name)
{
#if defined(_WIN32)
    const std::string variable(name);
    return ::_putenv_s(variable.c_str(), "") == 0;
#elif defined(__unix__) || defined(__APPLE__)
    return ::unsetenv(std::string(name).c_str()) == 0;
#else
    (void)name;
    return false;
#endif
}

}
