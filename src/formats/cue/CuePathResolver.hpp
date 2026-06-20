#ifndef TAGREADER_FORMATS_CUE_CUEPATHRESOLVER_HPP
#define TAGREADER_FORMATS_CUE_CUEPATHRESOLVER_HPP

#include <filesystem>

namespace tagreader_cue
{
enum class CuePathResolutionStatus
{
    Resolved,
    Missing,
    EmptyName,
    AbsolutePath,
    PathEscape,
    Symlink,
    Directory,
    NonRegular,
    SelfReference,
};

struct CuePathResolution
{
    CuePathResolutionStatus status{CuePathResolutionStatus::Missing};
    std::filesystem::path resolvedPath;
};

CuePathResolution ResolveCueFileReference(const std::filesystem::path &cuePath, const std::filesystem::path &fileName);
}

#endif
