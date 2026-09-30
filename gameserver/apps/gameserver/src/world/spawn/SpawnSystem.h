#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "../components/TransformComponents.h"

namespace gs::game {

class Zone;
class TerrainService;
struct MobSpawnPoint;
struct MobTypeDefinition;

// Creates flecs-native entities from spawn data. Never builds a parallel
// SimMob/SimPlayer object: components are set directly on the new entity and
// only session association (socket, static character data) goes into the
// zone's player binding. Callers must hold zone write ownership.
class SpawnSystem {
public:
    static void SpawnPlayer(Zone& zone,
                            std::shared_ptr<gs::network::Session> session,
                            gs::db::Character character,
                            const Position& position,
                            std::uint32_t net_id);

    static void SpawnMob(Zone& zone,
                         const MobSpawnPoint& spawn,
                         std::size_t spawn_point_index,
                         const MobTypeDefinition& type,
                         const Position& position,
                         std::uint32_t net_id);
};

} // namespace gs::game
