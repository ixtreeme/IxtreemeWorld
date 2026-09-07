#pragma once

#include <cstdint>

// Canonical shared identity for the partition subsystem. Kept in its own
// header so zone/partition/directory code can share ZoneId/RegionId/
// PartitionState WITHOUT include cycles (this header depends on nothing
// project-local).
namespace gs::game {

using ZoneId = std::uint32_t;
using RegionId = std::uint16_t;

// Simulation-topology state of a zone. Only ActiveLeaf-equivalent nodes
// (leaf + simulating) may hold authoritative entities. Internal (split)
// nodes keep metadata so the tree stays navigable; Retired nodes hold no
// entities and receive no routing.
enum class PartitionState : std::uint8_t {
    Leaf = 0,
    SplitPending = 1,
    Merging = 2,
    Retired = 3,
};

} // namespace gs::game
