#ifndef TAGREADER_CORE_COVERBUDGET_HPP
#define TAGREADER_CORE_COVERBUDGET_HPP

#include <cstdint>

namespace tagreader_core
{
// Returns true when debiting `bytes` from `accumulatedBytes` would exceed
// `budgetBytes` — the cumulative per-read encoded source-cover byte budget.
// Overflow-safe. A zero budget disables source-art reads: any non-zero `bytes`
// exceeds it, while zero bytes never do.
constexpr bool ExceedsCoverSourceBudget(std::uint64_t accumulatedBytes,
                                        std::uint64_t bytes,
                                        std::uint64_t budgetBytes) noexcept
{
    if (bytes > budgetBytes)
    {
        return true;
    }
    return accumulatedBytes > budgetBytes - bytes;
}
}

#endif
