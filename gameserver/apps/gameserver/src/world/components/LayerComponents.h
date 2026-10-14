#pragma once

#include <cstdint>

#include <flecs.h>

// 3D-5A layered-world presence. Authoritative, lives ONLY on the flecs
// entity (and travels in transfers / border snapshots as plain data).
//
// An entity WITHOUT this component is a legacy terrain entity: its z comes
// from the terrain heightfield and it is indexed in spatial volume 0. An
// entity WITH it stands on the cooked support plane of exactly one walkable
// volume of the package's layered world; its z is derived from that plane,
// its moves are checked against the cooked clearance (3D-4B) and a volume
// change only happens across a proven portal. The volume is never inferred
// from z or from an XY point: it is set by explicit admission and changed
// only by a proven portal crossing.
namespace gs::game {

struct LayerPresence {
    std::uint32_t volume_id = 0; // never 0 while the component is present
    std::uint32_t layer_id = 0;
};

// Spatial-index volume of an entity: its presence volume, or 0 (terrain).
inline std::uint32_t SpatialVolumeOf(flecs::entity entity)
{
    if (!entity.is_valid()) {
        return 0;
    }
    const auto* presence = entity.try_get<LayerPresence>();
    return presence != nullptr ? presence->volume_id : 0;
}

} // namespace gs::game
