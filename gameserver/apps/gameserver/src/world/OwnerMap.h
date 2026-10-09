#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "common/Types.h"

#include "distributed/GlobalEntityId.h"
#include "distributed/RuntimeIds.h"
#include "distributed/ZoneLocation.h"

// Stable cross-zone ownership record: which location currently owns the
// authoritative entity for a session, and under which global identity.
// Classification: DERIVED INDEX (session -> authority location). The
// authoritative gameplay state lives in flecs; this map only routes inputs
// and commands to the owning zone. Single-threaded: supervisor only.
//
// `entity` + `location` are the routing truth and the only fields that may
// ever cross a process boundary. `zone_index` + `net_id` are an explicit
// fast-path cache for the zone that PHYSICALLY hosts the entity in this
// process (always true single-process; also true under logical-distribution
// emulation, where assignment is logical only). The debug validator asserts
// the caches resolve to the live entity. This keeps the hot local path at
// one array index instead of a directory round-trip.
namespace gs::game {

struct OwnerInfo {
    GlobalEntityId entity;
    ZoneLocation location;
    std::size_t zone_index = 0;
    std::uint32_t net_id = 0;
};

using OwnerMap = std::unordered_map<gs::common::SessionId, OwnerInfo>;

} // namespace gs::game
