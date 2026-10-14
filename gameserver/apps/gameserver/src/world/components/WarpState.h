#pragma once

#include <cstdint>
#include "../terrain/TerrainRequest.h"

namespace gs::game {

// World-package warp ids, never zone ids. Carried with entity ownership.
// Rearm requires leaving ALL source volumes and the one-second cooldown.
// One pending request per player, cancelled on leave/despawn, expires in 5s
// of authoritative simulation time. No deferred callback owns an entity.
struct WarpState {
    std::uint32_t pending_id = 0;
    float pending_seconds = 0.0f;
    float cooldown_seconds = 0.0f;
    bool armed = true;
    bool transfer_pending = false;
    TerrainRequestHandle terrain_request;
    std::uint32_t completed = 0;
    std::uint32_t refused = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t timed_out = 0;
};

} // namespace gs::game
