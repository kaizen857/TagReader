#ifndef TAGREADER_COVER_SIDECARCOVER_HPP
#define TAGREADER_COVER_SIDECARCOVER_HPP

#include "cover/CoverCache.hpp"

namespace tagreader_cover
{
// Discovers sidecar cover candidates next to `context.filePath`, reads them in
// the existing deterministic priority order, debits each actually-read
// candidate's encoded bytes against the shared per-read source budget, and
// exports the requested outputs. Returns empty CoverPaths when no candidate
// produced artwork. Throws CoverProcessingError (SidecarDiscoveryFailed /
// SidecarEntryLimitExceeded / SourceBudgetExceeded / cover cache failures)
// according to the active CoverProcessingOptions.
CoverPaths ExportSidecarCover(tagreader_core::ReadContext &context);
}

#endif
