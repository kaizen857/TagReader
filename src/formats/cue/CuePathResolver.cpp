#include "formats/cue/CuePathResolver.hpp"

#include <iterator>
#include <system_error>

namespace tagreader_cue
{
namespace
{
bool HasEscapeComponent(const std::filesystem::path &path)
{
    for (const auto &component : path)
    {
        const std::filesystem::path part = component;
        if (part == "." || part == "..")
        {
            return true;
        }
    }
    return false;
}

CuePathResolution MakeResolution(CuePathResolutionStatus status, const std::filesystem::path &path = {})
{
    return CuePathResolution{status, path};
}
}

CuePathResolution ResolveCueFileReference(const std::filesystem::path &cuePath, const std::filesystem::path &fileName)
{
    if (fileName.empty())
    {
        return MakeResolution(CuePathResolutionStatus::EmptyName);
    }

    if (fileName.is_absolute() || fileName.has_root_path())
    {
        return MakeResolution(CuePathResolutionStatus::AbsolutePath);
    }

    if (HasEscapeComponent(fileName))
    {
        return MakeResolution(CuePathResolutionStatus::PathEscape);
    }

    const std::filesystem::path cueDirectory = cuePath.parent_path();
    std::filesystem::path resolved = cueDirectory;

    std::error_code ec;
    for (auto it = fileName.begin(); it != fileName.end(); ++it)
    {
        resolved /= *it;

        const std::filesystem::file_status status = std::filesystem::symlink_status(resolved, ec);
        if (ec)
        {
            if (ec == std::make_error_code(std::errc::no_such_file_or_directory))
            {
                return MakeResolution(CuePathResolutionStatus::Missing, resolved);
            }
            return MakeResolution(CuePathResolutionStatus::NonRegular, resolved);
        }

        if (std::filesystem::is_symlink(status))
        {
            return MakeResolution(CuePathResolutionStatus::Symlink, resolved);
        }

        const bool lastComponent = std::next(it) == fileName.end();
        if (!lastComponent && !std::filesystem::is_directory(status))
        {
            return MakeResolution(CuePathResolutionStatus::NonRegular, resolved);
        }
    }

    ec.clear();
    if (!std::filesystem::exists(resolved, ec) || ec)
    {
        return MakeResolution(CuePathResolutionStatus::Missing, resolved);
    }

    ec.clear();
    if (std::filesystem::equivalent(resolved, cuePath, ec) && !ec)
    {
        return MakeResolution(CuePathResolutionStatus::SelfReference, resolved);
    }

    ec.clear();
    const std::filesystem::file_status finalStatus = std::filesystem::symlink_status(resolved, ec);
    if (ec)
    {
        return MakeResolution(CuePathResolutionStatus::NonRegular, resolved);
    }
    if (std::filesystem::is_symlink(finalStatus))
    {
        return MakeResolution(CuePathResolutionStatus::Symlink, resolved);
    }
    if (std::filesystem::is_directory(finalStatus))
    {
        return MakeResolution(CuePathResolutionStatus::Directory, resolved);
    }
    if (!std::filesystem::is_regular_file(finalStatus))
    {
        return MakeResolution(CuePathResolutionStatus::NonRegular, resolved);
    }

    return MakeResolution(CuePathResolutionStatus::Resolved, resolved);
}
}
