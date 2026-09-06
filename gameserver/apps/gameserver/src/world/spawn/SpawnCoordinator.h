#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "common/Types.h"
#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "../components/TransformComponents.h"
#include "../distributed/Routing.h"
#include "../distributed/RuntimeIds.h"
#include "../OwnerMap.h"
#include "../replication/NetworkEntityId.h"
#include "MobPrototypeRegistry.h"
#include "RespawnSystem.h"
#include "SpawnLoader.h"

// Entity lifecycle for players and mobs: spawn, despawn, timed respawn.
// Creates flecs-native entities via SpawnSystem; never keeps runtime mob
// state (flecs owns it); spawn POINT data is immutable configuration.
// Owns the NetId allocator and the respawn queue. Single-threaded: called
// only from the supervisor thread; zone-side effects travel as zone commands.
// Concrete class, no interface (single implementation).
namespace gs::game {

class Zone;
class ZoneManager;
class TerrainService;
class WorldDirectory;
class WorldMessageRouter;

struct DebugSpawnOverride {
    float x = 0.0f;
    float y = 0.0f;
};

class SpawnCoordinator {
public:
    using SendFn =
        std::function<void(std::shared_ptr<gs::network::Session>, std::vector<std::uint8_t>)>;

    SpawnCoordinator(boost::asio::io_context& io,
                     ZoneManager& zones,
                     TerrainService& terrain,
                     const mx::map::WorldLogic& world_logic,
                     OwnerMap& owners,
                     WorldMessageRouter& router,
                     WakeFn wake,
                     SendFn send,
                     RuntimeIdentity identity,
                     WorldDirectory& directory);

    // Loads prototypes + spawn points and spawns the configured mobs.
    void Initialize(const std::string& map_root, const std::string& mob_types_config);

    void Spawn(std::shared_ptr<gs::network::Session> session,
               gs::db::Character character,
               std::optional<DebugSpawnOverride> debug_spawn,
               std::uint32_t world_tick);
    void Despawn(gs::common::SessionId session_id);

    bool SpawnMobFromSpawnPoint(std::size_t spawn_point_index);
    // Thread-safe: appends a spawn point usable by later spawns/respawns.
    // Also serves future runtime (GM) spawn control, not just benchmarks.
    void AddSpawnPoint(const MobSpawnPoint& point);
    void ProcessRespawns(float dt);
    void ScheduleRespawn(std::size_t spawn_point_index, float delay_sec)
    {
        respawns_.Push(spawn_point_index, delay_sec);
    }

    MobPrototypeRegistry& MobTypes() noexcept
    {
        return mob_types_;
    }
    std::size_t RespawnsPending() const
    {
        return respawns_.PendingCount();
    }
    std::uint64_t RespawnsTotal() const noexcept
    {
        return respawns_total_;
    }
    void ClearRespawns()
    {
        respawns_.Clear();
    }

private:
    void PostToOwner(std::size_t zone_index, std::function<void(Zone&)> command);

    Position ResolveSpawnPosition(const gs::db::Character& character,
                                  std::optional<DebugSpawnOverride> debug_spawn,
                                  gs::common::SessionId session_id);
    bool IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn);
    void SpawnConfiguredMobs();

    boost::asio::io_context& io_;
    ZoneManager& zones_;
    TerrainService& terrain_;
    const mx::map::WorldLogic& world_logic_;
    OwnerMap& owners_;
    WorldMessageRouter& router_;
    WakeFn wake_;
    SendFn send_;
    RuntimeIdentity identity_;
    WorldDirectory& directory_;

    MobPrototypeRegistry mob_types_;
    mutable std::mutex spawn_points_mutex_;
    std::vector<MobSpawnPoint> spawn_points_;
    RespawnSystem respawns_;
    NetIdAllocator net_ids_;
    std::uint64_t respawns_total_ = 0;
};

} // namespace gs::game
