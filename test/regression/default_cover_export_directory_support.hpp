#pragma once

#include <filesystem>
#include <string_view>

namespace default_cover_test
{
bool RunDefaultFallbackIgnoresLegacyRootSymlink(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath);
bool RunXdgRuntimeDefaultIsPreferred(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath);
bool RunDefaultRootSymlinkIsRejected(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath);
bool RunExplicitSymlinkDirectoryIsRejected(const std::filesystem::path &workspaceRoot, const std::filesystem::path &samplePath);
std::filesystem::path MakeCaseRoot(std::string_view caseName);
bool PrepareCaseRoot(const std::filesystem::path &root);
}
