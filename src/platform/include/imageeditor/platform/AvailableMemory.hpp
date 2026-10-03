#pragma once
#include <cstdint>
#include <optional>

namespace imageeditor::platform {
// Currently available host memory, constrained by inherited cgroup-v2 limits.
// Already resident documents are reflected in this headroom; do not subtract
// their memory again. Unknown platforms return no estimate.
std::optional<std::uint64_t> availableMemoryBytes();
// Allow an operation 75% of available memory, leaving room for other work.
// Unknown headroom uses a conservative fallback rather than an unlimited load.
std::uint64_t availableWorkingMemoryBytes();
// OS-reported current process resident set, not virtual size or peak usage.
// Dedicated GPU allocations are not included. No estimate on unsupported hosts.
std::optional<std::uint64_t> processResidentMemoryBytes();
}
