#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "common/Types.h"

// Stable cross-zone ownership record: which zone currently owns the
// authoritative entity for a session, and under which global NetId.
// Classification: DERIVED INDEX (session -> authority location). The
// authoritative gameplay state lives in flecs; this map only routes inputs
// and commands to the owning zone. Single-threaded: supervisor only.
namespace gs::game {

struct OwnerInfo {
    std::size_t zone_index = 0;
    std::uint32_t net_id = 0;
};

using OwnerMap = std::unordered_map<gs::common::SessionId, OwnerInfo>;

} // namespace gs::game
