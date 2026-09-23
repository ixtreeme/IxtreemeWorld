#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <flecs.h>

#include "../components/MovementComponents.h"
#include "../components/TransformComponents.h"

// Phase 5C canonical transform record: 19 bytes, recipient-independent
// (NetId + position + quantized heading + move state). Serialized once per
// entity version per tick and referenced by every interested recipient.
namespace gs::game {
inline constexpr std::size_t kTransformRecordSize = 19;
using TransformRecord = std::array<std::uint8_t, kTransformRecordSize>;
} // namespace gs::game

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
    // entity is zone-LOCAL runtime state (this zone's flecs world only, never
    // stored or sent anywhere else). The cross-process unit -- now across
    // threads via publish buffers, later across processes via BorderSnapshot
    // transport (§23) -- is `snapshot` alone: stable NetId + plain data.
    flecs::entity entity;
    BorderEntitySnapshot snapshot;
    // Phase 5A incremental maintenance bookkeeping (zone-local, never
    // published): which neighbor zone published this ghost (first-wins in
    // neighbor order, matching the historic dedupe semantics), and the
    // reconcile generation that last saw it. Used to keep unchanged ghosts
    // alive across ticks and to drop ghosts whose source stopped publishing.
    std::uint32_t source_zone_id = 0;
    std::uint32_t last_seen_generation = 0;
};

// Exact field equality for publish-buffer content comparison (phase 5A): the
// producer only advances its publish generation when the buffer actually
// changed, so consumers can skip unchanged neighbors entirely. Exact, not a
// hash: a hash collision would silently drop a state change.
inline bool SameBorderSnapshot(const BorderEntitySnapshot& lhs,
                               const BorderEntitySnapshot& rhs) noexcept
{
    return lhs.net_id == rhs.net_id && lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y && lhs.position.z == rhs.position.z &&
           lhs.heading.angle == rhs.heading.angle && lhs.move_state == rhs.move_state &&
           lhs.class_id == rhs.class_id && lhs.mob_type_id == rhs.mob_type_id &&
           lhs.level == rhs.level && lhs.hp_current == rhs.hp_current &&
           lhs.hp_max == rhs.hp_max && lhs.name == rhs.name;
}

} // namespace gs::game
