#pragma once

#include <cstddef>
#include <cstdint>

#include "common/Types.h"

#include "../OwnerMap.h"

// Cross-zone ownership transfer orchestration (supervisor side).
// Collects MigrateTo marks set by zone-local movement steps and executes the
// transfer: source authority stopped -> transfer snapshot -> destination
// entity created -> destination owns. At most one zone is authoritative for a
// NetId at any time; flecs handles never cross zones (see EntityTransfer).
// Single-threaded: called only from the supervisor loop when no zone tick is
// in progress. Concrete class, no interface (single implementation).
namespace gs::game {

class ZoneManager;
class TerrainService;

class MigrationCoordinator {
public:
    MigrationCoordinator(ZoneManager& zones, TerrainService& terrain, OwnerMap& owners);

    void ProcessMigrations(std::uint32_t world_tick);

private:
    void ExecuteMigration(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          std::uint32_t net_id,
                          std::uint32_t world_tick);

    ZoneManager& zones_;
    TerrainService& terrain_;
    OwnerMap& owners_;
};

} // namespace gs::game
