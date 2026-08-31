#pragma once

#include <string>

namespace realmheart::ui {

// Deterministic, compositor-free report of the supported resolution contracts
// plus the asset paths that each display tier is expected to consume. This is
// intentionally diagnostic-only: runtime surfaces still select their tier from
// the monitor actually assigned to them after realization.
[[nodiscard]] std::string resolution_compatibility_report();

} // namespace realmheart::ui
