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

#include "activity/SpatialActivityField.h"
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
#include "partition/PartitionConfig.h"
#include "partition/PartitionMetrics.h"
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
    // Cumulative simulation-LOD work totals (§22, §25). Never reset (unlike
    // the per-second diag windows); the bench derives per-second rates.
    struct LodWorkTotals {
        std::uint64_t ai_updates = 0;
        std::uint64_t move_updates = 0;
        std::uint64_t promotions = 0;
        std::uint64_t demotions = 0;
        std::uint64_t wakes = 0;
        std::uint64_t eval_us = 0;
    };
    LodWorkTotals LodWorkTotalsSnapshot() const noexcept
    {
        LodWorkTotals totals;
        totals.ai_updates = lod_ai_total_.load(std::memory_order_relaxed);
        totals.move_updates = lod_mv_total_.load(std::memory_order_relaxed);
        totals.promotions = lod_prom_total_.load(std::memory_order_relaxed);
        totals.demotions = lod_dem_total_.load(std::memory_order_relaxed);
        totals.wakes = lod_wake_total_.load(std::memory_order_relaxed);
        totals.eval_us = lod_eval_us_total_.load(std::memory_order_relaxed);
        return totals;
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
    // Current activity field generation (immutable snapshot, never null).
    // Readers copy the shared_ptr; no further synchronization needed.
    std::shared_ptr<const ActivityGrid> ActivitySnapshot() const
    {
        return activity_field_.Snapshot();
    }
    ActivityMetricsSnapshot ActivityMetrics() const
    {
        return activity_field_.Metrics();
    }
    // Strict field-vs-brute-force audit (§31): runs in the same quiescent
    // window as RequestValidation, over a deterministic mob sample. For
    // STATIC scenarios the field must match brute force exactly (stronger-
    // or-equal); use only where entities don't move mid-audit. Fills
    // per-sample results for exact per-mob asserts. max_samples 0 = all.
    void RequestActivityValidation(std::size_t max_samples = 64);
    // Collects the detailed activity audit outcome. Returns true only when
    // ready AND passed; out_error carries the first mismatch otherwise.
    bool TryTakeActivitySamples(std::vector<ActivitySampleResult>& out_samples,
                                std::string& out_error);
    double SupervisorAvgMs() const;
    ZoneWorkerPool::Utilization WorkerUtilization() const;
    std::size_t MigrationQuarantined() const;
    MigrationId LastCommittedMigration() const;
    MigrationMetrics::Snapshot MigrationMetrics() const;
    PartitionMetrics::Snapshot PartitionMetricsSnapshot() const
    {
        return partition_metrics_.TakeSnapshot();
    }
    const PartitionConfig& EffectivePartitionConfig() const noexcept
    {
        return effective_partition_config_;
    }
    const LodConfig& EffectiveLodConfig() const noexcept
    {
        return effective_lod_config_;
    }

    // Applies a (validated, clamped) partition configuration: monitor +
    // scheduler thresholds and region split limits. Call before Start, or
    // between supervisor passes. Logs every correction + the effective set.
    void ConfigurePartition(const PartitionConfig& config);

    // Applies a (validated, clamped) simulation LOD configuration: tier
    // bubbles, frequencies, demotion graces. Ticks read it through the tick
    // context; the scheduler sleep rule follows the same switch. Call
    // before Start, or between supervisor passes.
    void ConfigureSimulationLod(const LodConfig& config);

    // Test seams (benchmarks/admin). Enqueued to the supervisor thread like
    // any other command; they run the FULL transactional path, only the
    // load-predicate gate is bypassed.
    void PostForceSplit(ZoneId zone_id);
    void PostForceMerge(ZoneId parent_node_id);
    // Arms transfer-failure injection: the next N snapshot-stage / apply-stage
    // transfers fail deterministically (§17-18). `succeed_first` lets that
    // many transfers complete before the failure fires (mid-batch abort).
    void InjectTransferFailuresForTest(int snapshot_failures, int apply_failures,
                                       int succeed_first = 0);

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
    // One full split transaction: Plan -> Create(staged) -> Transfer ->
    // Validate -> Commit, with rollback + AbortSplit on any failure (§3-4).
    // `forced` bypasses load predicates (test seams); the machinery is
    // otherwise identical. Returns true on commit.
    bool RunSplitTransaction(ZoneId zone_id, bool forced);
    // One full merge transaction: Plan -> CreateTarget(staged) -> Transfer
    // -> Validate -> CommitTreeCollapse, with rollback + AbortMerge (§5).
    bool RunMergeTransaction(ZoneId parent_node_id, bool forced);
    // Supervisor-side bodies behind PostForceSplit/PostForceMerge.
    void ExecuteForcedSplit(ZoneId zone_id);
    void ExecuteForcedMerge(ZoneId parent_node_id);
    // True when the migration queue holds a request touching the zone
    // (source or target): transactions never start under a racing migration.
    bool ZoneHasPendingMigration(ZoneId zone_id) const;
    // Moves one resident (player or mob) between two LOCAL zones reusing the
    // migration authority-transfer primitive (snapshot -> apply -> release).
    // Acquires both zones' write guards in index order (same convention as
    // MigrationCoordinator), so the transfer never races worker ticks.
    bool TransferResident(std::size_t source_zone_index,
                          std::size_t target_zone_index,
                          ZoneLocation target_location,
                          std::uint32_t net_id);
    // Same transfer with the caller already holding both write guards.
    // Apply-first ordering with an RAII destination rollback guard (§7-8):
    // the source is released only after the target fully accepted; any
    // failure before the commit point destroys the partial destination and
    // leaves the source untouched.
    //
    // COMMIT POINT (§10): source Grid.Remove + destruct + Unindex. Before
    // it, the destination is unobservable (both write guards held, no tick
    // in flight, staged zones unscheduled/unrouted); after it, the
    // destination is authoritative and OwnerMap is updated to follow.
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
    // World-space activity field (first Adaptive Simulation Fabric
    // foundation). Rebuilt ~1Hz on the supervisor from per-zone published
    // player sources; consumed via immutable snapshots by zone ticks
    // (LOD eval), the scheduler (sleep/wake) and validators/bench.
    SpatialActivityField activity_field_;
    std::chrono::steady_clock::time_point last_activity_build_{};
    PartitionMetrics partition_metrics_;
    PartitionConfig effective_partition_config_;
    LodConfig effective_lod_config_;

    // Failure-injection hooks for bench/test only (§17-18). Countdowns of
    // transfers to fail: snapshot-stage (before any mutation) or
    // apply-stage (after destination build, exercising the rollback guard).
    // `test_fail_after_count_` lets that many transfers succeed first, so
    // mid-batch aborts (non-empty reverse path) are reproducible.
    // Set from any thread, consumed supervisor-side.
    std::atomic<int> test_fail_snapshot_count_{0};
    std::atomic<int> test_fail_apply_count_{0};
    std::atomic<int> test_fail_after_count_{0};

    std::atomic<std::uint64_t> attacks_since_diag_{0};
    std::atomic<std::uint64_t> attacks_total_{0};
    std::atomic<std::uint64_t> deaths_total_{0};
    std::atomic<std::uint64_t> lod_ai_total_{0};
    std::atomic<std::uint64_t> lod_mv_total_{0};
    std::atomic<std::uint64_t> lod_prom_total_{0};
    std::atomic<std::uint64_t> lod_dem_total_{0};
    std::atomic<std::uint64_t> lod_wake_total_{0};
    std::atomic<std::uint64_t> lod_eval_us_total_{0};
    std::atomic<std::uint32_t> world_tick_{0};
    std::uint64_t supervisor_micros_since_diag_ = 0;
    std::atomic<std::uint64_t> supervisor_micros_total_{0};
    std::atomic<std::uint64_t> supervisor_samples_{0};

    std::atomic<bool> validation_requested_{false};
    mutable std::mutex validation_mutex_;
    std::string validation_result_;
    bool validation_ready_ = false;

    std::atomic<bool> activity_validation_requested_{false};
    std::atomic<std::size_t> activity_validation_max_samples_{64};
    mutable std::mutex activity_validation_mutex_;
    std::vector<ActivitySampleResult> activity_samples_;
    std::string activity_validation_error_;
    bool activity_samples_ready_ = false;
    bool activity_validation_ok_ = false;
};

} // namespace gs::game
