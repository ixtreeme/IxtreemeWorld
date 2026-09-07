#pragma once

#include <atomic>
#include <chrono>
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
#include "distributed/MigrationTransport.h"
#include "distributed/RuntimeIds.h"
#include "distributed/WorldDirectory.h"
#include "distributed/WorldMessageRouter.h"
#include "input/InputRouter.h"
#include "migration/MigrationCoordinator.h"
#include "migration/MigrationQueue.h"
#include "partition/ZoneLoadMonitor.h"
#include "spawn/SpawnCoordinator.h"
#include "terrain/TerrainService.h"
#include "zone/ZoneManager.h"
#include "zone/ZoneScheduler.h"
#include "zone/ZoneWorkerPool.h"
#include "zone/ZoneLoadMetrics.h"

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
    // identity defaults to the single-process deployment (node=1/process=1).
    explicit WorldRuntime(boost::asio::io_context& io, RuntimeIdentity identity = {});
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
    // Routing table + router. Non-const access is for emulation/admin setup
    // (SetAssignment, SetRemoteMode) and routing self-tests -- never for
    // gameplay writes from other threads.
    WorldDirectory& Directory() noexcept
    {
        return directory_;
    }
    RuntimeIdentity Identity() const noexcept
    {
        return identity_;
    }
    WorldMessageRouter& Router() noexcept
    {
        return router_;
    }
    // Test seam: direct queue access for routing self-tests (stale/duplicate
    // injection). Production producers enqueue via gameplay systems.
    MigrationQueue& TestMigrationQueue() noexcept
    {
        return migration_queue_;
    }
    // Logical-distribution emulation (§33): partition local zones across K
    // logical processes in this binary (round-robin by zone index). Routing
    // and migration treat non-local partitions as remote-emulated while all
    // delivery stays local. Benchmark/balancer use only.
    void EmulateDistribution(std::uint32_t logical_processes);
    // Cluster-scheduler input snapshot (also used by benchmarks). Cheap,
    // non-destructive reads; values are point-in-time approximations.
    ProcessLoadSnapshot CollectProcessLoad() const;
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
    MigrationId LastCommittedMigration() const;
    MigrationMetrics::Snapshot MigrationMetrics() const;

private:
    void Enqueue(std::function<void()> command);
    void Run();
    void TickZone(std::size_t zone_index);
    ZoneTickContext BuildZoneTickContext();
    void DrainGlobalCommands();
    // Slow topology control plane (~Hz): load monitor update + at most the
    // due split/merge transactions. Runs only when no zone tick is in
    // flight, so partition mutation never races worker threads.
    void ExecutePartitionControl();
    // Moves one resident (player or mob) between two LOCAL zones reusing the
    // migration authority-transfer primitive (snapshot -> apply -> release).
    // Acquires both zones' write guards in index order (same convention as
    // MigrationCoordinator), so the transfer never races worker ticks.
    bool TransferResident(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          ZoneLocation target_location,
                          std::uint32_t net_id);
    // Same transfer with the caller already holding both write guards.
    // Apply-first ordering: the source is released only after the target
    // accepted, so a failure leaves the source untouched (no rollback/quarantine
    // needed on this always-local path).
    bool TransferResidentLocked(Zone& source_zone,
                                Zone& target_zone,
                                std::size_t target_zone_index,
                                ZoneLocation target_location,
                                std::uint32_t net_id);

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

    RuntimeIdentity identity_;
    WorldDirectory directory_;
    WorldMessageRouter router_;
    MigrationTransport migration_transport_;

    TerrainService terrain_;
    mx::map::WorldLogic world_logic_;
    OwnerMap owners_by_session_;
    MigrationQueue migration_queue_;

    SpawnCoordinator spawn_;
    MigrationCoordinator migration_;
    InputRouter inputs_;

    ZoneLoadMonitor load_monitor_;
    // Slow control-plane cadence (§42): topology decisions at ~1 Hz while
    // simulation runs at 20 Hz.
    std::chrono::steady_clock::time_point last_partition_control_{};

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
