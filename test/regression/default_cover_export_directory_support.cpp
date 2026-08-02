#include "default_cover_export_directory_support.hpp"

#include "TagReader.hpp"
#include "catch2_regression_support.hpp"
#include "catch2_sample_support.hpp"

#include <libavutil/log.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX 1
#include <unistd.h>
#else
#define TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX 0
#endif

namespace default_cover_test
{
namespace
{
using tagreader_test_support::PrepareCleanDirectory;
using tagreader_test_support::PrepareSymlink;
using tagreader_test_support::SetEnvironment;
using tagreader_test_support::UnsetEnvironment;

bool Expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "expectation failed: " << message << '\n';
        return false;
    }
    return true;
}

std::size_t CountPngFiles(const std::filesystem::path &root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        return 0;
    }

    std::size_t count = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
        {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
        {
            ++count;
        }
    }
    return count;
}

bool PathIsUnder(const std::filesystem::path &path, const std::filesystem::path &root)
{
    std::error_code ec;
    const std::filesystem::path normalizedPath = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        return false;
    }
    ec.clear();
    const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
    {
        return false;
    }

    const auto mismatch = std::mismatch(normalizedRoot.begin(), normalizedRoot.end(), normalizedPath.begin(), normalizedPath.end());
    return mismatch.first == normalizedRoot.end();
}

std::filesystem::path FallbackDefaultCoverDir(const std::filesystem::path &tmpRoot)
{
#if TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX
    return tmpRoot / ("tagreader-covers-" + std::to_string(static_cast<unsigned long long>(::geteuid())));
#else
    return tmpRoot / "tagreader-covers-private";
#endif
}

}

std::filesystem::path MakeCaseRoot(std::string_view caseName)
{
    return std::filesystem::temp_directory_path() / "tagreader_default_cover_export_directory_catch2" / std::string(caseName);
}

bool PrepareCaseRoot(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    return !ec;
}

bool RunDefaultFallbackIgnoresLegacyRootSymlink(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath)
{
    const std::filesystem::path tmpRoot = workspaceRoot / "tmp";
    const std::filesystem::path hijackTarget = workspaceRoot / "hijack";
    const std::filesystem::path legacyDefaultRoot = tmpRoot / "tagreader-covers";
    const std::filesystem::path privateDefaultRoot = FallbackDefaultCoverDir(tmpRoot);

    if (!PrepareCleanDirectory(tmpRoot) || !PrepareCleanDirectory(hijackTarget) || !PrepareSymlink(hijackTarget, legacyDefaultRoot) || !SetEnvironment("TMPDIR", tmpRoot) || !UnsetEnvironment("XDG_RUNTIME_DIR"))
    {
        return false;
    }

    try
    {
        const MusicTag tag = TagReader::Read(samplePath);
        const std::filesystem::path coverPath = tag.coverPath();
        return Expect(!coverPath.empty(), "default fallback should export a cover") &&
               Expect(std::filesystem::is_regular_file(coverPath), "default fallback cover should exist") &&
               Expect(PathIsUnder(coverPath, privateDefaultRoot), "default fallback cover should use UID-private directory") &&
               Expect(!PathIsUnder(coverPath, hijackTarget), "default fallback must not write through legacy root symlink") &&
               Expect(CountPngFiles(hijackTarget) == 0, "legacy root symlink hijack target should stay empty") &&
               Expect(CountPngFiles(privateDefaultRoot) == 2, "UID-private default directory should contain one full and one thumbnail PNG");
    }
    catch (const std::exception &ex)
    {
        std::cerr << "default fallback read failed: " << ex.what() << '\n';
        return false;
    }
}

bool RunXdgRuntimeDefaultIsPreferred(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath)
{
    const std::filesystem::path runtimeRoot = workspaceRoot / "runtime";
    const std::filesystem::path runtimeDefaultRoot = runtimeRoot / "tagreader-covers";

    if (!PrepareCleanDirectory(runtimeRoot) || !SetEnvironment("TMPDIR", workspaceRoot / "tmp") || !SetEnvironment("XDG_RUNTIME_DIR", runtimeRoot))
    {
        return false;
    }

    try
    {
        const MusicTag tag = TagReader::Read(samplePath);
        const std::filesystem::path coverPath = tag.coverPath();
        return Expect(!coverPath.empty(), "XDG runtime default should export a cover") &&
               Expect(std::filesystem::is_regular_file(coverPath), "XDG runtime default cover should exist") &&
               Expect(PathIsUnder(coverPath, runtimeDefaultRoot), "XDG_RUNTIME_DIR/tagreader-covers should be preferred") &&
               Expect(CountPngFiles(runtimeDefaultRoot) == 2, "XDG runtime default directory should contain one full and one thumbnail PNG");
    }
    catch (const std::exception &ex)
    {
        std::cerr << "XDG runtime default read failed: " << ex.what() << '\n';
        return false;
    }
}

bool RunDefaultRootSymlinkIsRejected(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath)
{
    const std::filesystem::path runtimeRoot = workspaceRoot / "runtime-symlink";
    const std::filesystem::path hijackTarget = workspaceRoot / "runtime-hijack";
    const std::filesystem::path symlinkDefaultRoot = runtimeRoot / "tagreader-covers";

    if (!PrepareCleanDirectory(runtimeRoot) || !PrepareCleanDirectory(hijackTarget) || !PrepareSymlink(hijackTarget, symlinkDefaultRoot) || !SetEnvironment("XDG_RUNTIME_DIR", runtimeRoot))
    {
        return false;
    }

    bool rejected = false;
    try
    {
        (void)TagReader::Read(samplePath);
    }
    catch (const std::exception &ex)
    {
        const std::string error = ex.what();
        rejected = error.find("cover export") != std::string::npos && (error.find("symlink") != std::string::npos || error.find("symbolic link") != std::string::npos);
    }

    return Expect(rejected, "default root symlink should be rejected before cover export") && Expect(CountPngFiles(hijackTarget) == 0, "rejected default root symlink should not receive PNG files");
}

bool RunExplicitSymlinkDirectoryIsRejected(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath)
{
    const std::filesystem::path explicitTarget = workspaceRoot / "explicit-target";
    const std::filesystem::path explicitLink = workspaceRoot / "explicit-link";

    if (!PrepareCleanDirectory(explicitTarget) || !PrepareSymlink(explicitTarget, explicitLink))
    {
        return false;
    }

    bool rejected = false;
    try
    {
        (void)TagReader::Read(samplePath, explicitLink);
    }
    catch (const std::exception &ex)
    {
        const std::string error = ex.what();
        rejected = error.find("cover export") != std::string::npos && (error.find("symlink") != std::string::npos || error.find("symbolic link") != std::string::npos);
    }

    return Expect(rejected, "explicit symlink directory should be rejected before cover export") && Expect(CountPngFiles(explicitTarget) == 0, "rejected explicit symlink target should stay empty");
}
}
