#pragma once

#include <cstdint>

// Network replication bookkeeping (zone-local, never authoritative gameplay
// state). Phase 5B: the world-global tick at which the entity's replicated
// transform (position / heading / move_state) last changed. Recipients store
// the last version they were sent; a transform record is generated only when
// the entity's version is newer (or on the staggered periodic refresh).
namespace gs::game {

struct TransformVersion {
    std::uint32_t tick = 0;
};

} // namespace gs::game
