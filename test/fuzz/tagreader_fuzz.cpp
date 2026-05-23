#include "TagReader.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
std::filesystem::path FuzzRoot()
{
    return std::filesystem::temp_directory_path() / "tagreader_fuzz";
}

std::filesystem::path FuzzInputPath()
{
    return FuzzRoot() / "input.bin";
}

std::filesystem::path FuzzCoverDir()
{
    return FuzzRoot() / "covers";
}

void CleanupFuzzFiles()
{
    std::error_code ec;
    std::filesystem::remove(FuzzInputPath(), ec);
    std::filesystem::remove_all(FuzzCoverDir(), ec);
}

bool WriteTempInput(const std::uint8_t *data, std::size_t size, const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    output.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return output.good();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    CleanupFuzzFiles();

    const std::filesystem::path inputPath = FuzzInputPath();
    if (!WriteTempInput(data, size, inputPath))
    {
        CleanupFuzzFiles();
        return 0;
    }

    try
    {
        (void)TagReader::Read(inputPath, FuzzCoverDir());
    }
    catch (...)
    {
    }

    CleanupFuzzFiles();
    return 0;
}
