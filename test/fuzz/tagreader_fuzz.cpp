#include "TagReader.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace
{
std::filesystem::path FuzzRoot()
{
    static const std::filesystem::path root = []
    {
        if (const char *overrideRoot = std::getenv("TAGREADER_FUZZ_ROOT"); overrideRoot != nullptr && overrideRoot[0] != '\0')
        {
            return std::filesystem::path(overrideRoot);
        }

        static std::atomic_uint64_t rootCounter{0};
        const auto processId = static_cast<unsigned long long>(::getpid());
        const auto threadId = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        const auto sequence = static_cast<unsigned long long>(rootCounter.fetch_add(1, std::memory_order_relaxed));

        return std::filesystem::temp_directory_path() /
               ("tagreader_fuzz_" + std::to_string(processId) + "_" + std::to_string(threadId) + "_" + std::to_string(sequence));
    }();

    return root;
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
    std::filesystem::remove(FuzzRoot(), ec);
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
