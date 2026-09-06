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

#include <boost/asio/io_context.hpp>

#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "components/MovementComponents.h"
#include "OwnerMap.h"
#include "input/InputRouter.h"
#include "migration/MigrationCoordinator.h"
#include "spawn/SpawnCoordinator.h"
#include "terrain/TerrainService.h"
#include "zone/ZoneManager.h"
#include "zone/ZoneScheduler.h"
#include "zone/ZoneWorkerPool.h"
#include "OwnerMap.h"

// Thin world orchestrator. Owns ONLY:
//  - startup/shutdown, supervisor thread, global command routing
//  - zone lifetimes (ZoneManager), tick scheduling (ZoneScheduler + pool)
//  - high-level orchestration of the Run loop + global diagnostics
//
// Domain work lives in concrete coordinators, not here:
//  - entity lifecycle (spawn/despawn/respawn): SpawnCoordinator
//  - cross-zone ownership transfer: MigrationCoordinator
//  - client input fan-out: InputRouter
namespace gs::game {

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
    void Enqueue(std::function<void()> command);
    void Run();
    void TickZone(std::size_t zone_index);
    ZoneTickContext BuildZoneTickContext();
    void DrainGlobalCommands();

    boost::asio::io_context& io_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::thread::id sim_thread_id_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> commands_;

    ZoneManager zones_;
    ZoneScheduler scheduler_;
    ZoneWorkerPool workers_;

    TerrainService terrain_;
    mx::map::WorldLogic world_logic_;
    OwnerMap owners_by_session_;

    SpawnCoordinator spawn_;
    MigrationCoordinator migration_;
    InputRouter inputs_;

    std::atomic<std::uint64_t> attacks_since_diag_{0};
    std::atomic<std::uint64_t> deaths_total_{0};
    std::atomic<std::uint32_t> world_tick_{0};
};

} // namespace gs::game
