#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "components/MovementComponents.h"
#include "OwnerMap.h"
#include "debug/WorldValidator.h"
#include "input/InputRouter.h"
#include "migration/MigrationCoordinator.h"
#include "migration/MigrationQueue.h"
#include "spawn/SpawnCoordinator.h"
#include "terrain/TerrainService.h"
#include "zone/ZoneManager.h"
#include "zone/ZoneScheduler.h"
#include "zone/ZoneWorkerPool.h"

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

    // Runtime spawn control (benchmarks/GM tooling). Thread-safe; the actual
    // spawn executes supervisor-side like a respawn.
    void AddMobSpawnPoint(const MobSpawnPoint& point);
    void RequestMobSpawn(std::size_t spawn_point_index);

    // Read-only observability for benchmarks/admin (no gameplay writes).
    const ZoneManager& Zones() const noexcept
    {
        return zones_;
    }
    const OwnerMap& Owners() const noexcept
    {
        return owners_by_session_;
    }
    const MigrationQueue& Migrations() const noexcept
    {
        return migration_queue_;
    }
    std::uint64_t DeathsTotal() const noexcept
    {
        return deaths_total_.load(std::memory_order_relaxed);
    }
    std::uint64_t AttacksTotal() const noexcept
    {
        return attacks_total_.load(std::memory_order_relaxed);
    }
    std::uint32_t WorldTick() const noexcept
    {
        return world_tick_.load(std::memory_order_relaxed);
    }

    // Debug/test consistency audit. Never called on the hot path. Non-const
    // because the audit walks live zone state; the caller must quiesce ticks.
    bool ValidateConsistency(std::string& out_error);

    // Operational audit hook for benchmarks/admin tooling. RequestValidation
    // asks the supervisor loop to run the consistency audit in its naturally
    // quiescent window (no zone tick in progress, before new ones are
    // scheduled); TryTakeValidationResult collects the outcome. Zero hot-path
    // cost when unused.
    void RequestValidation();
    bool TryTakeValidationResult(std::string& out_result);
    double SupervisorAvgMs() const;
    ZoneWorkerPool::Utilization WorkerUtilization() const;
    std::size_t MigrationQuarantined() const;

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
    MigrationQueue migration_queue_;

    SpawnCoordinator spawn_;
    MigrationCoordinator migration_;
    InputRouter inputs_;

    std::atomic<std::uint64_t> attacks_since_diag_{0};
    std::atomic<std::uint64_t> attacks_total_{0};
    std::atomic<std::uint64_t> deaths_total_{0};
    std::atomic<std::uint32_t> world_tick_{0};
    std::uint64_t supervisor_micros_since_diag_ = 0;
    std::atomic<std::uint64_t> supervisor_micros_total_{0};
    std::atomic<std::uint64_t> supervisor_samples_{0};

    std::atomic<bool> validation_requested_{false};
    mutable std::mutex validation_mutex_;
    std::string validation_result_;
    bool validation_ready_ = false;
};

} // namespace gs::game
