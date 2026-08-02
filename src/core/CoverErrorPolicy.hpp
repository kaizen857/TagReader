#ifndef TAGREADER_CORE_COVERERRORPOLICY_HPP
#define TAGREADER_CORE_COVERERRORPOLICY_HPP

#include "TagReader.hpp"

namespace tagreader_core
{
struct ReadContext;

enum class CoverErrorAction
{
    NotACoverError,
    Ignored,
    Propagated,
};

// Classifies an exception observed at a cover-capable boundary (e.g. a metadata
// parser exporting embedded cover art). Never throws. Defined in
// src/core/TagPipeline.cpp.
CoverErrorAction ClassifyCoverFailure(const std::exception &ex, const ReadContext &context) noexcept;
}

#endif
