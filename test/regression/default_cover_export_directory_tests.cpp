#include "TagReader.hpp"

#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/log.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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

namespace
{
bool CommandSucceeds(const std::string &command)
{
    return std::system(command.c_str()) == 0;
}

bool Expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "expectation failed: " << message << '\n';
        return false;
    }

    return true;
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
    if (!output.good())
    {
        std::cerr << "failed to write file: " << path.string() << '\n';
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void AppendBytes(std::vector<std::uint8_t> &bytes, std::string_view text)
{
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void AppendU32BE(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendSyncSafe32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 21) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((value >> 14) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(value & 0x7F));
}

std::vector<std::uint8_t> OneByOnePng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
        0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

std::vector<std::uint8_t> Id3v23Frame(std::string_view frameId, const std::vector<std::uint8_t> &payload)
{
    std::vector<std::uint8_t> bytes;
    AppendBytes(bytes, frameId);
    AppendU32BE(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> Id3v23Tag(const std::vector<std::uint8_t> &frames)
{
    std::vector<std::uint8_t> bytes{'I', 'D', '3', 3, 0, 0};
    AppendSyncSafe32(bytes, static_cast<std::uint32_t>(frames.size()));
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    return bytes;
}

bool GenerateBaseMp3(const std::filesystem::path &path)
{
    if (!CommandSucceeds("command -v ffmpeg >/dev/null 2>&1"))
    {
        std::cerr << "ffmpeg CLI not found; default cover export directory tests require generated audio\n";
        return false;
    }

    const std::string command = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2 -codec:a libmp3lame -write_id3v1 0 -id3v2_version 0 \"" + path.string() + "\"";
    if (!CommandSucceeds(command))
    {
        std::cerr << "failed to generate base MP3 sample with ffmpeg\n";
        return false;
    }

    return true;
}

bool PrependId3Tag(const std::filesystem::path &basePath, const std::filesystem::path &outputPath, const std::vector<std::uint8_t> &frames)
{
    const std::vector<std::uint8_t> base = ReadBinaryFile(basePath);
    if (base.empty())
    {
        std::cerr << "failed to read base MP3 sample: " << basePath.string() << '\n';
        return false;
    }

    std::vector<std::uint8_t> output = Id3v23Tag(frames);
    output.insert(output.end(), base.begin(), base.end());
    return WriteBinaryFile(outputPath, output);
}

bool GenerateCoverSample(const std::filesystem::path &samplePath)
{
    const std::filesystem::path basePath = samplePath.parent_path() / "base.mp3";
    const std::vector<std::uint8_t> validPng = OneByOnePng();
    const std::vector<std::uint8_t> apicPayload = [&]
    {
        std::vector<std::uint8_t> payload{0};
        AppendBytes(payload, "image/png");
        payload.insert(payload.end(), {0, 3, 0});
        payload.insert(payload.end(), validPng.begin(), validPng.end());
        return payload;
    }();

    return GenerateBaseMp3(basePath) && PrependId3Tag(basePath, samplePath, Id3v23Frame("APIC", apicPayload));
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

bool SetEnvironment(std::string_view name, const std::filesystem::path &value)
{
#if TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX
    if (::setenv(std::string(name).c_str(), value.c_str(), 1) != 0)
    {
        std::cerr << "failed to set environment variable: " << name << '\n';
        return false;
    }
    return true;
#else
    (void)name;
    (void)value;
    return false;
#endif
}

bool UnsetEnvironment(std::string_view name)
{
#if TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX
    if (::unsetenv(std::string(name).c_str()) != 0)
    {
        std::cerr << "failed to unset environment variable: " << name << '\n';
        return false;
    }
    return true;
#else
    (void)name;
    return false;
#endif
}

bool PrepareCleanDirectory(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    ec.clear();
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        std::cerr << "failed to create directory " << path.string() << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

bool PrepareSymlink(const std::filesystem::path &target, const std::filesystem::path &link)
{
    std::error_code ec;
    std::filesystem::remove(link, ec);
    ec.clear();
    std::filesystem::create_directory_symlink(target, link, ec);
    if (ec)
    {
        std::cerr << "failed to create symlink " << link.string() << " -> " << target.string() << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

bool DefaultFallbackIgnoresLegacyRootSymlink(const std::filesystem::path &samplePath)
{
    const std::filesystem::path tmpRoot = "/tmp/tagreader-s1-tmp";
    const std::filesystem::path hijackTarget = "/tmp/tagreader-s1-hijack";
    const std::filesystem::path legacyDefaultRoot = tmpRoot / "tagreader-covers";
    const std::filesystem::path privateDefaultRoot = FallbackDefaultCoverDir(tmpRoot);

    if (!PrepareCleanDirectory(tmpRoot) || !PrepareCleanDirectory(hijackTarget) ||
        !PrepareSymlink(hijackTarget, legacyDefaultRoot) || !SetEnvironment("TMPDIR", tmpRoot) || !UnsetEnvironment("XDG_RUNTIME_DIR"))
    {
        return false;
    }

    bool passed = true;
    try
    {
        const MusicTag tag = TagReader::Read(samplePath);
        const std::filesystem::path coverPath = tag.coverPath();
        passed = Expect(!coverPath.empty(), "default fallback should export a cover") && passed;
        passed = Expect(std::filesystem::is_regular_file(coverPath), "default fallback cover should exist") && passed;
        passed = Expect(PathIsUnder(coverPath, privateDefaultRoot), "default fallback cover should use UID-private directory") && passed;
        passed = Expect(!PathIsUnder(coverPath, hijackTarget), "default fallback must not write through legacy root symlink") && passed;
        passed = Expect(CountPngFiles(hijackTarget) == 0, "legacy root symlink hijack target should stay empty") && passed;
        passed = Expect(CountPngFiles(privateDefaultRoot) == 1, "UID-private default directory should contain one PNG") && passed;
        std::cout << "S1 default-fallback-cover-path=" << coverPath.string() << '\n';
    }
    catch (const std::exception &ex)
    {
        std::cerr << "default fallback read failed: " << ex.what() << '\n';
        passed = false;
    }

    return passed;
}

bool XdgRuntimeDefaultIsPreferred(const std::filesystem::path &samplePath)
{
    const std::filesystem::path tmpRoot = "/tmp/tagreader-s1-tmp";
    const std::filesystem::path runtimeRoot = "/tmp/tagreader-s1-runtime";
    const std::filesystem::path runtimeDefaultRoot = runtimeRoot / "tagreader-covers";

    if (!PrepareCleanDirectory(runtimeRoot) || !SetEnvironment("TMPDIR", tmpRoot) || !SetEnvironment("XDG_RUNTIME_DIR", runtimeRoot))
    {
        return false;
    }

    bool passed = true;
    try
    {
        const MusicTag tag = TagReader::Read(samplePath);
        const std::filesystem::path coverPath = tag.coverPath();
        passed = Expect(!coverPath.empty(), "XDG runtime default should export a cover") && passed;
        passed = Expect(std::filesystem::is_regular_file(coverPath), "XDG runtime default cover should exist") && passed;
        passed = Expect(PathIsUnder(coverPath, runtimeDefaultRoot), "XDG_RUNTIME_DIR/tagreader-covers should be preferred") && passed;
        passed = Expect(CountPngFiles(runtimeDefaultRoot) == 1, "XDG runtime default directory should contain one PNG") && passed;
        std::cout << "S1 xdg-runtime-cover-path=" << coverPath.string() << '\n';
    }
    catch (const std::exception &ex)
    {
        std::cerr << "XDG runtime default read failed: " << ex.what() << '\n';
        passed = false;
    }

    return passed;
}

bool DefaultRootSymlinkIsRejected(const std::filesystem::path &samplePath)
{
    const std::filesystem::path runtimeRoot = "/tmp/tagreader-s1-runtime-symlink";
    const std::filesystem::path hijackTarget = "/tmp/tagreader-s1-runtime-hijack";
    const std::filesystem::path symlinkDefaultRoot = runtimeRoot / "tagreader-covers";

    if (!PrepareCleanDirectory(runtimeRoot) || !PrepareCleanDirectory(hijackTarget) ||
        !PrepareSymlink(hijackTarget, symlinkDefaultRoot) || !SetEnvironment("XDG_RUNTIME_DIR", runtimeRoot))
    {
        return false;
    }

    bool rejected = false;
    std::string error;
    try
    {
        (void)TagReader::Read(samplePath);
    }
    catch (const std::exception &ex)
    {
        error = ex.what();
        rejected = error.find("cover export") != std::string::npos &&
                   (error.find("symlink") != std::string::npos || error.find("symbolic link") != std::string::npos);
    }

    bool passed = true;
    passed = Expect(rejected, "default root symlink should be rejected before cover export") && passed;
    passed = Expect(CountPngFiles(hijackTarget) == 0, "rejected default root symlink should not receive PNG files") && passed;
    std::cout << "S1 default-root-symlink-error=" << error << '\n';
    return passed;
}

bool ExplicitSymlinkDirectoryIsRejected(const std::filesystem::path &samplePath)
{
    const std::filesystem::path explicitTarget = "/tmp/tagreader-s2-explicit-target";
    const std::filesystem::path explicitLink = "/tmp/tagreader-s2-explicit-link";

    if (!PrepareCleanDirectory(explicitTarget) || !PrepareSymlink(explicitTarget, explicitLink))
    {
        return false;
    }

    bool rejected = false;
    std::string error;
    try
    {
        (void)TagReader::Read(samplePath, explicitLink);
    }
    catch (const std::exception &ex)
    {
        error = ex.what();
        rejected = error.find("cover export") != std::string::npos &&
                   (error.find("symlink") != std::string::npos || error.find("symbolic link") != std::string::npos);
    }

    bool passed = true;
    passed = Expect(rejected, "explicit symlink directory should be rejected before cover export") && passed;
    passed = Expect(CountPngFiles(explicitTarget) == 0, "rejected explicit symlink target should stay empty") && passed;
    std::cout << "S2 explicit-symlink-error=" << error << '\n';
    return passed;
}
}

int main()
{
    av_log_set_level(AV_LOG_QUIET);

#if !TAGREADER_DEFAULT_COVER_TEST_HAS_POSIX
    std::cerr << "default cover export directory tests require POSIX setenv/symlink support\n";
    return 1;
#else
    const std::filesystem::path sampleRoot = "/tmp/tagreader-s1-samples";
    std::error_code ec;
    std::filesystem::remove_all(sampleRoot, ec);
    ec.clear();
    std::filesystem::create_directories(sampleRoot, ec);
    if (ec)
    {
        std::cerr << "failed to create sample directory: " << ec.message() << '\n';
        return 1;
    }

    const std::filesystem::path samplePath = sampleRoot / "cover-policy.mp3";
    if (!GenerateCoverSample(samplePath))
    {
        return 1;
    }

    bool passed = true;
    passed = DefaultFallbackIgnoresLegacyRootSymlink(samplePath) && passed;
    passed = XdgRuntimeDefaultIsPreferred(samplePath) && passed;
    passed = DefaultRootSymlinkIsRejected(samplePath) && passed;
    passed = ExplicitSymlinkDirectoryIsRejected(samplePath) && passed;

    if (passed)
    {
        std::cout << "S1 default cover export directory PASS\n";
        return 0;
    }

    std::cerr << "S1 default cover export directory FAIL\n";
    return 1;
#endif
}
