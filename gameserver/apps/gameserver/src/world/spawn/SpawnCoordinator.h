#pragma once
#include "../terrain/TerrainRequest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
#include "PresenceRegistry.h"
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
                     SendFn send_and_close,
                     RuntimeIdentity identity,
                     WorldDirectory& directory);

    // Loads prototypes, takes the (package-validated) spawn points and spawns
    // the configured mobs.
    // spawn_now = false: only load the types + take the points (streaming
    // worlds spawn them in budget-sized batches, WorldRuntime).
    void Initialize(std::vector<MobSpawnPoint> spawn_points,
                    const std::string& mob_types_config,
                    bool spawn_now = true);
    // Synthetic-world seam (integrated-scale benchmarks): load only the mob
    // prototypes, then let the caller register spawn points and trigger the
    // bulk spawn explicitly. Production uses Initialize above.
    void LoadMobTypes(const std::string& mob_types_config);
    // Drops every registered spawn point (synthetic-world setup must not
    // inherit the map's spawn points).
    void ClearSpawnPoints();
    // Spawns every configured spawn point (point.count each). Synchronous,
    // supervisor/setup only: callers must guarantee no zone tick is in
    // flight. Returns the number of mobs actually spawned.
    std::size_t SpawnAllConfiguredMobs();

    // Enforces the world presence invariant (H4): a character already held by
    // another session is refused with EnterWorldReject::alreadyInWorld and
    // the requesting session is closed; nothing is spawned.
    void Spawn(std::shared_ptr<gs::network::Session> session,
               gs::db::Character character,
               std::optional<DebugSpawnOverride> debug_spawn,
               std::uint32_t world_tick, TerrainRequestHandle terrain_request = {});
    // Releases the session's presence (if it holds one) and its entity.
    void Despawn(gs::common::SessionId session_id);

    const PresenceRegistry& Presence() const noexcept
    {
        return presence_;
    }
    void ClearPresence()
    {
        presence_.Clear();
    }

    // needs_terrain (optional): set when the only obstacle was a chunk that
    // is not resident (demanded; the caller retries later).
    bool SpawnMobFromSpawnPoint(std::size_t spawn_point_index, bool* needs_terrain = nullptr);
    // All `count` mobs of one spawn point (setup / streaming batches).
    std::size_t SpawnPointMobs(std::size_t spawn_point_index);
    std::vector<MobSpawnPoint> SpawnPointsSnapshot() const;
    // MAP-3: chunk demand sink (streaming worlds; supervisor thread).
    void SetTerrainDemand(std::function<void(std::uint32_t)> demand)
    {
        terrain_demand_ = std::move(demand);
    }
    std::uint64_t RespawnsDroppedNoTerrain() const noexcept
    {
        return respawns_dropped_no_terrain_.load(std::memory_order_relaxed);
    }
    // Mob spawns (initial or respawn) refused because the chosen position's
    // chunk is published INVALID (permanent load failure): never retried in
    // this package generation, never placed on guessed ground.
    std::uint64_t MobSpawnsRefusedInvalidTerrain() const noexcept
    {
        return mob_spawns_invalid_terrain_.load(std::memory_order_relaxed);
    }
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
    // Enters refused because no valid spawn position exists (thread-safe).
    std::uint64_t PlayerSpawnRefusals() const noexcept
    {
        return player_spawn_refusals_.load(std::memory_order_relaxed);
    }
    void ClearRespawns()
    {
        respawns_.Clear();
    }

private:
    void PostToOwner(std::size_t zone_index, std::function<void(Zone&)> command);

    // Player spawn rule (MAP-2 review, explicit, no clamp / no fallback):
    //   1. debug override (dev builds only) when finite, inside the world,
    //      walkable, owned by a zone and with a terrain height;
    //   2. else the centre of the package's FIRST player spawn region (file
    //      order) under the same conditions -- the validator guarantees it
    //      exists and is walkable, so with resident terrain this holds;
    //   3. else nullopt: the enter is refused (EnterWorldReject serverError).
    // The stored DB position is NOT a candidate: the gameserver never writes
    // it back (no persistence path) and it carries no world identity (map_id
    // is not tied to the package world_id), so it could name a point of a
    // different world. Resuming at a stored position needs both first.
    std::optional<Position> ResolveSpawnPosition(std::optional<DebugSpawnOverride> debug_spawn,
                                                 gs::common::SessionId session_id);
    // Inside the world, walkable, owned by a zone and with a height.
    bool IsValidSpawnPoint(float x, float y) const;
    bool IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn);

    boost::asio::io_context& io_;
    ZoneManager& zones_;
    TerrainService& terrain_;
    const mx::map::WorldLogic& world_logic_;
    OwnerMap& owners_;
    WorldMessageRouter& router_;
    WakeFn wake_;
    SendFn send_;
    SendFn send_and_close_;
    RuntimeIdentity identity_;
    PresenceRegistry presence_;
    WorldDirectory& directory_;

    MobPrototypeRegistry mob_types_;
    mutable std::mutex spawn_points_mutex_;
    std::vector<MobSpawnPoint> spawn_points_;
    // Initial activation is idempotent; respawn has its own existing path.
    std::unordered_map<std::size_t, std::uint32_t> initially_spawned_;
    RespawnSystem respawns_;
    NetIdAllocator net_ids_;
    std::uint64_t respawns_total_ = 0;
    std::atomic<std::uint64_t> player_spawn_refusals_{0};
    std::function<void(std::uint32_t)> terrain_demand_;
    static constexpr std::uint32_t kMaxTerrainDeferrals = 40; // x 0.5 s
    std::unordered_map<std::size_t, std::uint32_t> terrain_deferrals_;
    std::atomic<std::uint64_t> respawns_dropped_no_terrain_{0};
    std::atomic<std::uint64_t> mob_spawns_invalid_terrain_{0};
};

} // namespace gs::game
