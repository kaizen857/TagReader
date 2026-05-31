#ifndef TAGREADER_MEDIA_FFMPEGSESSION_HPP
#define TAGREADER_MEDIA_FFMPEGSESSION_HPP

#include "core/ReadContext.hpp"

#include <filesystem>

namespace tagreader_media
{
void RegisterAllFormatsIfNeeded();
tagreader_core::ReadContext OpenContext(const std::filesystem::path &filePath);
}

#endif
