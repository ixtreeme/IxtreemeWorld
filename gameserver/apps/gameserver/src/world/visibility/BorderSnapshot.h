#pragma once

#include <cstdint>
#include <string>

#include <flecs.h>

#include "../components/MovementComponents.h"
#include "../components/TransformComponents.h"

// Read-only cross-zone representation of a border-band resident.
// Produced by the owning zone (BorderPublisher), consumed by neighbor zones
// (GhostSystem). Snapshots are plain data: no pointers, no entity handles,
// no access back into the owner's flecs storage.
namespace gs::game {

struct BorderEntitySnapshot {
    std::uint32_t net_id = 0;
    Position position;
    Heading heading;
    MoveState move_state = MoveState::Idle;
    std::string name;
    std::uint16_t class_id = 0;
    std::uint32_t mob_type_id = 0;
    std::uint32_t level = 1;
    float hp_current = 1.0f;
    float hp_max = 1.0f;
};

struct GhostRecord {
    flecs::entity entity;
    BorderEntitySnapshot snapshot;
};

} // namespace gs::game
