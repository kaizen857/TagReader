#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tagreader_test_support
{
bool CommandSucceeds(const std::string &command);
bool HasFfmpeg();
bool WriteBinaryFile(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes);
bool WriteTextFile(const std::filesystem::path &path, std::string_view text);
std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path &path);
void AppendBytes(std::vector<std::uint8_t> &bytes, std::string_view text);
void AppendU24BE(std::vector<std::uint8_t> &bytes, std::uint32_t value);
void AppendU32BE(std::vector<std::uint8_t> &bytes, std::uint32_t value);
void AppendU32LE(std::vector<std::uint8_t> &bytes, std::uint32_t value);
void AppendSyncSafe32(std::vector<std::uint8_t> &bytes, std::uint32_t value);
std::uint32_t ReadU24BE(const std::vector<std::uint8_t> &bytes, std::size_t offset);
std::vector<std::uint8_t> Bytes(std::string_view text);
std::filesystem::path TemporaryArtifactRoot(std::string_view caseName);
bool PrepareCleanDirectory(const std::filesystem::path &path);
bool PrepareSymlink(const std::filesystem::path &target, const std::filesystem::path &link);
bool SetEnvironment(std::string_view name, const std::filesystem::path &value);
bool UnsetEnvironment(std::string_view name);
}
