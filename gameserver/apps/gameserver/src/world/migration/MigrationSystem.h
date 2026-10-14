#pragma once

#include <flecs.h>

#include "../components/TransformComponents.h"

// Zone-local migration detection: marks entities that have left the zone
// bounds (with hysteresis, targeting only neighbor zones) and enqueues a
// MigrationRequest for the supervisor. The mark (MigrateTo component) is the
// persistent "needs migration" state; the queue is the work list. This module
// never moves entities itself.
namespace gs::game {

class Zone;
class ZoneManager;
class MigrationQueue;

class MigrationSystem {
public:
    static void UpdateMarker(Zone& zone,
                             ZoneManager& zones,
                             MigrationQueue* queue,
                             std::uint32_t net_id,
                             flecs::entity entity,
                             const Position& position);
};

} // namespace gs::game
