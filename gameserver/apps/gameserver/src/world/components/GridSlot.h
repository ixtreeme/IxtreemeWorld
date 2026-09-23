#pragma once

#include <cstdint>

// Phase 5D spatial-grid bookkeeping (zone-local, derived index state; never
// authoritative gameplay state). The grid stores each entity's position in
// its cell entry so the AOI radius scan is a sequential read instead of a
// random component lookup per candidate; this slot is how the movement path
// updates that stored position in O(1) without a per-cell linear search.
namespace gs::game {

struct GridSlot {
    std::int64_t cell_key = 0;   // packed spatial cell key
    std::uint32_t index = 0;     // slot inside the cell's entry vector
};

} // namespace gs::game
