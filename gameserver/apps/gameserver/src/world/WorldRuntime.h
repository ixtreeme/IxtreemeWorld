#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "components/MovementComponents.h"
#include "replication/NetworkEntityId.h"
#include "spawn/MobPrototypeRegistry.h"
#include "spawn/RespawnSystem.h"
#include "spawn/SpawnLoader.h"
#include "terrain/TerrainService.h"
#include "zone/ZoneManager.h"
#include "zone/ZoneScheduler.h"
#include "zone/ZoneWorkerPool.h"

// Thin world orchestrator. Owns ONLY:
//  - startup/shutdown, supervisor thread, global command routing
//  - zone lifetimes (ZoneManager), tick scheduling (ZoneScheduler + pool)
//  - cross-zone migration orchestration (plans + ownership transfer)
//  - spawn/despawn routing, input fan-out, respawn scheduling
//  - global diagnostics
//
// It deliberately contains NO movement/AI/combat/spatial/ghost/terrain/
// packet-format logic; those live in their own modules. Target size:
// orchestration only, not gameplay.
namespace gs::game {

struct DebugSpawnOverride {
    float x = 0.0f;
    float y = 0.0f;
};

class WorldRuntime {
public:
    explicit WorldRuntime(boost::asio::io_context& io);
    ~WorldRuntime();

    WorldRuntime(const WorldRuntime&) = delete;
    WorldRuntime& operator=(const WorldRuntime&) = delete;

    void Start();
    void Stop();

    void PostSpawn(std::shared_ptr<gs::network::Session> session,
                   gs::db::Character character,
                   std::optional<DebugSpawnOverride> debug_spawn = std::nullopt);
    void PostDespawn(gs::common::SessionId session_id);
    void PostMoveInput(gs::common::SessionId session_id,
                       std::uint32_t sequence,
                       float dir_angle,
                       MoveState state);
    void PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id);

private:
    struct OwnerInfo {
        std::size_t zone_index = 0;
        std::uint32_t net_id = 0;
    };
    struct MoveInput {
        gs::common::SessionId session_id = 0;
        std::uint32_t sequence = 0;
        float dir_angle = 0.0f;
        MoveState state = MoveState::Idle;
    };
    struct AttackInput {
        gs::common::SessionId session_id = 0;
        std::uint32_t target_net_id = 0;
    };

    void Enqueue(std::function<void()> command);
    void Run();
    void TickZone(std::size_t zone_index);
    ZoneTickContext BuildZoneTickContext();
    void DrainGlobalCommands();
    void ProcessMigrations();
    void ExecuteMigration(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          std::uint32_t net_id);
    void ProcessRespawns(float dt);
    void DrainMoveInputs();
    void DrainAttackInputs();
    void EnqueueZoneCommand(std::size_t zone_index, ZoneCommandQueue::Command command);

    Position ResolveSpawnPosition(const gs::db::Character& character,
                                  std::optional<DebugSpawnOverride> debug_spawn,
                                  gs::common::SessionId session_id);
    bool IsValidDebugSpawnOverride(const DebugSpawnOverride& debug_spawn);

    void Spawn(std::shared_ptr<gs::network::Session> session,
               gs::db::Character character,
               std::optional<DebugSpawnOverride> debug_spawn);
    void Despawn(gs::common::SessionId session_id);

    void SpawnConfiguredMobs();
    bool SpawnMobFromSpawnPoint(std::size_t spawn_point_index);

    boost::asio::io_context& io_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::thread::id sim_thread_id_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> commands_;

    std::mutex input_mutex_;
    std::vector<MoveInput> pending_inputs_;
    std::mutex attack_mutex_;
    std::vector<AttackInput> pending_attacks_;

    ZoneManager zones_;
    ZoneScheduler scheduler_;
    ZoneWorkerPool workers_;

    TerrainService terrain_;
    mx::map::WorldLogic world_logic_;
    MobPrototypeRegistry mob_types_;
    std::vector<MobSpawnPoint> spawn_points_;
    RespawnSystem respawns_;
    NetIdAllocator net_ids_;
    std::unordered_map<gs::common::SessionId, OwnerInfo> owners_by_session_;

    std::atomic<std::uint64_t> attacks_since_diag_{0};
    std::atomic<std::uint64_t> deaths_total_{0};
    std::atomic<std::uint64_t> respawns_total_{0};
    std::atomic<std::uint32_t> world_tick_{0};
};

} // namespace gs::game
