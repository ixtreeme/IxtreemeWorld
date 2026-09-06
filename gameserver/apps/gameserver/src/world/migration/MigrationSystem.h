#pragma once

#include <flecs.h>

#include "../components/TransformComponents.h"

// Zone-local migration detection: marks entities that have left the zone
// bounds (with hysteresis, targeting only neighbor zones). The mark is
// consumed by the supervisor, which performs the actual cross-zone ownership
// transfer. This module never moves entities itself.
namespace gs::game {

class Zone;
class ZoneManager;

class MigrationSystem {
public:
    static void UpdateMarker(Zone& zone, ZoneManager& zones, flecs::entity entity, const Position& position);
};

} // namespace gs::game
