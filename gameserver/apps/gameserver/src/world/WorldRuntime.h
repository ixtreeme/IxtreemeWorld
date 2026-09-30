#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "db/CharacterRepository.h"
#include "map/MapData.h"
#include "network/Session.h"

#include "activity/ContinuousLoadField.h"
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
#include "package/WorldPackageLoader.h"
#include "partition/PartitionConfig.h"
#include "partition/PartitionMetrics.h"
#include "partition/PartitionScoring.h"
#include "partition/ZoneLoadMonitor.h"
#include "replication/ReplicationConfig.h"
#include "spawn/SpawnCoordinator.h"
#include "terrain/TerrainService.h"
#include "terrain/NavigationService.h"
#include "terrain/TerrainStreamer.h"
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
    // Explicit synthetic world (world_mode = synthetic, benchmarks): flat
    // terrain with an explicit extent and a deterministic zone grid, so a
    // 100km world with hundreds of thousands of entities can be measured with
    // the production systems (no mocked simulation, no 100km^2 heightfield
    // asset). Never used as a fallback for a failed package load.
    struct SyntheticWorldConfig {
        float extent_m = 100000.0f;
        std::uint32_t zones_x = 8;
        std::uint32_t zones_y = 8;
        // Empty: no mob types, except in builds that define the bench-only
        // IXTREEME_DEFAULT_MOB_TYPES_CONFIG (worldbench).
        std::string mob_types_config;
        // Region grid over the world; the zone grid must divide into it.
        std::uint32_t regions_x = 1;
        std::uint32_t regions_y = 1;
    };

    // File-backed world from a validated package (MAP-1). The package is
    // loaded and checked BEFORE the runtime exists (WorldPackageLoader), so
    // construction cannot fail on world data. identity: node=1/process=1 is
    // the single-process deployment.
    // `layout` is the server's initial partition (regions + initial leaves)
    // over the package's bounds; an unusable layout throws
    // std::invalid_argument (main validates it before the runtime exists).
    // A streaming world (world.residency == Streaming) gets a terrain
    // streamer with `streaming` (budget, I/O threads, ...); ignored for an
    // eager world.
    WorldRuntime(boost::asio::io_context& io,
                 RuntimeIdentity identity,
                 LoadedWorld world,
                 const PartitionLayout& layout = {},
                 const TerrainStreamingConfig& streaming = {});
    // Synthetic world (explicit mode). Same systems, different bootstrap.
    WorldRuntime(boost::asio::io_context& io,
                 RuntimeIdentity identity,
                 const SyntheticWorldConfig& synthetic);
    ~WorldRuntime();

    // Setup seam: spawns every configured spawn point synchronously (bulk
    // entity creation through the production spawn path). Requires
    // quiescence: call before Start or between supervisor passes.
    std::size_t SpawnConfiguredMobsNow();

    WorldRuntime(const WorldRuntime&) = delete;
    WorldRuntime& operator=(const WorldRuntime&) = delete;

    void Start();
    void Stop();

    // --- Consistent world snapshots (MAP-0) ---------------------------------
    // Readers outside the supervisor (benchmarks, admin tools) must never walk
    // the zone table themselves: the supervisor splits, merges, retires,
    // reclaims and reuses zone slots concurrently, and zone ticks update the
    // per-zone gauges. A snapshot collector instead runs ON the supervisor
    // thread in a quiescent window -- no zone tick in flight, between two
    // topology operations -- so everything it reads (zone table, per-zone
    // diagnostics and LOD gauges, reclamation state, and anything the caller
    // samples alongside, e.g. process memory) comes from ONE mutation point.
    // The collector copies what it needs; the requester receives that copy
    // through the future. No production hot-path lock is involved.
    // Consistency scope: zone table, topology, per-zone gauges and every
    // supervisor-owned map belong to one mutation point. Commands already
    // queued INTO a zone (e.g. a spawn whose owner record exists but whose
    // entity the zone creates at its next tick) are not drained first -- that
    // stronger point is the consistency audit's (RequestValidation).
    struct SnapshotContext {
        const ZoneManager& zones;
        const OwnerMap& owners;       // session -> owning zone slot/id
        const PresenceRegistry& presence; // same quiescent mutation point
        std::uint64_t epoch = 0;      // strictly increasing, one per capture
        std::uint32_t world_tick = 0; // world tick at the capture point
        std::chrono::steady_clock::time_point captured_at{};
        ZoneManager::ReclaimStats reclaim{};
        std::shared_ptr<const ActivityGrid> activity;
        std::shared_ptr<const ActivityWakeFrame> wake_decision;
    };
    // Requests a capture; never blocks. While the runtime is not running
    // (before Start / after Stop) nothing else mutates the world, so the
    // collector runs immediately on the calling thread.
    template <class T>
    std::future<T> CaptureSnapshot(std::function<T(const SnapshotContext&)> collect)
    {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        EnqueueSnapshot([promise, collect = std::move(collect)](const SnapshotContext& ctx) {
            try {
                promise->set_value(collect(ctx));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        return future;
    }
    // Waits for a capture requested from another thread. Refuses (throws
    // std::logic_error) on the supervisor thread itself: that thread is the
    // one that would have to fulfil the request, so waiting there can only
    // deadlock. Returns false on timeout.
    template <class T>
    bool WaitSnapshot(std::future<T>& future, std::chrono::milliseconds timeout, T& out) const
    {
        if (std::this_thread::get_id() == sim_thread_id_.load(std::memory_order_acquire)) {
            throw std::logic_error("WaitSnapshot called on the supervisor thread");
        }
        if (future.wait_for(timeout) != std::future_status::ready) {
            return false;
        }
        out = future.get();
        return true;
    }
    struct SnapshotStats {
        std::uint64_t captures = 0;   // capture points served
        std::uint64_t requests = 0;   // collectors run
        std::uint64_t max_wait_us = 0; // longest request -> capture latency
    };
    SnapshotStats GetSnapshotStats() const noexcept
    {
        return SnapshotStats{snapshot_captures_.load(std::memory_order_relaxed),
                             snapshot_requests_.load(std::memory_order_relaxed),
                             snapshot_max_wait_us_.load(std::memory_order_relaxed)};
    }

    void PostSpawn(std::shared_ptr<gs::network::Session> session,
                   gs::db::Character character,
                   std::optional<DebugSpawnOverride> debug_spawn = std::nullopt,
                   TerrainRequestHandle terrain_request = {});
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
    // Immutable after construction (never reloaded on split/merge), so a
    // cross-thread read is safe.
    const TerrainService& Terrain() const noexcept
    {
        return terrain_;
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
    // Current continuous load field generation (immutable snapshot, never
    // null). Readers copy the shared_ptr; no further synchronization needed.
    // The field is world-space: its values are independent of the partition
    // topology by construction (§25).
    std::shared_ptr<const LoadGrid> LoadFieldSnapshot() const
    {
        return load_field_.Snapshot();
    }
    LoadFieldMetricsSnapshot LoadFieldMetrics() const
    {
        return load_field_.Metrics();
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
    // Phase 7 scheduler audit: explicit worker count (0 = auto). Must be set
    // before Start.
    // Diagnostics log cadence (default 1 s). Must be set before Start. The
    // partition control loop is independent of it (H5).
    void ConfigureDiagnosticsInterval(std::chrono::milliseconds interval) noexcept
    {
        diagnostics_interval_ = interval.count() > 0 ? interval : std::chrono::milliseconds(1000);
    }
    // Control decision-state transitions of the partition monitor (H5).
    ZoneLoadMonitor::ControlCounters PartitionControlCounters() const noexcept
    {
        return load_monitor_.GetControlCounters();
    }
    // World presence registry counters (H4): claims, releases, refused
    // duplicates and the live presence gauge (atomics, any thread).
    // MAP-3 terrain residency + query outcome counters (thread-safe
    // snapshots; streaming off -> the streamer part is empty).
    struct TerrainStats {
        bool streaming = false;
        TerrainStreamer::Stats streamer;
        TerrainQueryCounters queries;
        std::uint64_t safepoints = 0, drain_wait_us = 0;
        std::uint64_t commands_requested = 0, ingress_rejected = 0;
        std::uint64_t supervisor_cpu_us=0, drain_max_us=0;
    };
    TerrainStats GetTerrainStats() const;
    ZoneWorkerPool::Delays GetSchedulingDelays() const { return workers_.GetDelays(); }
    // Streaming seams (benchmarks / tools): demand the chunks around a point
    // (supervisor-side, like a consumer would) and reset the cache
    // generation (simulated package switch).
    void PostTerrainDemand(float x, float y, float radius_m);
    // Running-world production operation, callable from any thread. No
    // snapshot is needed; Ready pins the set through PostSpawn/Consume.
    TerrainRequestHandle PrepareTerrain(float x, float y, float radius_m, double timeout_seconds = 5.0);
    void BeginTerrainMeasurementWindow();
    void PostTerrainResetForTest();
    // Test seam: evicts every unpinned chunk nobody demanded for
    // `idle_seconds` (what budget pressure would do), freeing only in a
    // quiescent window like the regular path.
    void PostTerrainEvictIdleForTest(double idle_seconds);
    // Movement collision rules (slope / water); before Start.
    void ConfigureMovementRules(const MovementRules& rules) noexcept
    {
        terrain_.SetMovementRules(rules);
    }
    // MAP-3 navigation (infrastructure; no gameplay caller yet): a grid path
    // query executed on the supervisor with bounded work per pass. The id
    // is returned at once; poll NavigationResult (Pending until terminal).
    std::uint64_t PostNavigationRequest(const NavRequest& request);
    void PostNavigationCancel(std::uint64_t id);
    NavResult NavigationResult(std::uint64_t id) const
    {
        return navigation_.Result(id);
    }
    NavigationService::Stats NavigationStats() const
    {
        return navigation_.GetStats();
    }
    // Read-only world queries from any thread for tools/benches are NOT
    // safe while streaming (eviction frees in quiescent windows): use a
    // snapshot collector (supervisor) instead.
    PresenceRegistry::Stats PresenceStats() const noexcept
    {
        return spawn_.Presence().GetStats();
    }
    // Enters refused because no valid spawn position exists.
    std::uint64_t PlayerSpawnRefusals() const noexcept
    {
        return spawn_.PlayerSpawnRefusals();
    }
    // MAP-3 spawn outcomes on missing / unusable terrain.
    std::uint64_t MobSpawnsRefusedInvalidTerrain() const noexcept
    {
        return spawn_.MobSpawnsRefusedInvalidTerrain();
    }
    std::uint64_t RespawnsDroppedNoTerrain() const noexcept
    {
        return spawn_.RespawnsDroppedNoTerrain();
    }
    // Cumulative client-input path counters (posted/routed/applied/dropped).
    InputRouter::Stats InputStats() const
    {
        return inputs_.GetStats();
    }
    void ConfigureWorkers(std::size_t count) noexcept
    {
        requested_workers_ = count;
    }
    // Worker-level scheduling snapshot: per-worker work/tasks, the scheduler
    // counters and the wall time spent with at least one tick in flight.
    struct SchedulerSnapshot {
        std::size_t workers = 0;
        std::uint64_t waves = 0;
        std::uint64_t due_zones = 0;
        std::uint64_t enqueued = 0;
        std::uint64_t sleeping_skips = 0;
        std::uint64_t cas_failures = 0;
        std::uint64_t schedule_micros = 0;
        std::uint64_t worker_work_micros = 0;
        std::uint64_t worker_idle_micros = 0;
        std::uint64_t busy_phase_micros = 0;
        std::uint64_t idle_phase_micros = 0;
        std::vector<std::uint64_t> worker_work;
        std::vector<std::uint64_t> worker_tasks;
    };
    SchedulerSnapshot SchedulerStats() const;
    ZoneScheduler::WakeMetrics ActivityWakeMetrics() const { return scheduler_.GetWakeMetrics(); }
    std::size_t MigrationQuarantined() const;
    MigrationId LastCommittedMigration() const;
    MigrationMetrics::Snapshot MigrationMetrics() const;
    PartitionMetrics::Snapshot PartitionMetricsSnapshot() const
    {
        return partition_metrics_.TakeSnapshot();
    }
    // H9: retired zone slot reclamation (published by the supervisor).
    ZoneManager::ReclaimStats ZoneReclaimStats() const
    {
        std::lock_guard lock(reclaim_stats_mutex_);
        return reclaim_stats_;
    }
    const PartitionConfig& EffectivePartitionConfig() const noexcept
    {
        return effective_partition_config_;
    }
    const LodConfig& EffectiveLodConfig() const noexcept
    {
        return effective_lod_config_;
    }
    const ReplicationConfig& EffectiveReplicationConfig() const noexcept
    {
        return effective_replication_config_;
    }
    const LoadFieldConfig& EffectiveLoadFieldConfig() const noexcept
    {
        return effective_load_field_config_;
    }
    const PartitionScoringConfig& EffectivePartitionScoringConfig() const noexcept
    {
        return scorer_.GetConfig();
    }

    // Adaptive split scoring, read-only (OBSERVE->SCORE view for diagnostics,
    // benchmarks and admin tooling). Deterministic for the current field
    // generation; never mutates topology. Point-in-time live read, following
    // the same convention as Zones()/CollectProcessLoad(): it is NOT
    // synchronized against a concurrent topology mutation, so treat the
    // result as a near-supervisor-cycle approximation (the supervisor path
    // itself is exact).
    SplitRecommendation ScorePartition(ZoneId zone_id) const;
    // Adaptive merge scoring for one real quadtree sibling group (parent node
    // id). Read-only, deterministic, never mutates topology.
    MergeRecommendation ScoreMerge(ZoneId parent_id) const;
    // Structured decision log (bounded, newest last). Contains executed
    // splits/merges and rate-limited why-not records.
    std::vector<PartitionDecisionRecord> PartitionDecisionLog() const;

    // Applies a (validated, clamped) partition configuration: monitor +
    // scheduler thresholds and region split limits. Call before Start, or
    // between supervisor passes. Logs every correction + the effective set.
    void ConfigurePartition(const PartitionConfig& config);

    // Applies a (validated, clamped) simulation LOD configuration: tier
    // bubbles, frequencies, demotion graces. Ticks read it through the tick
    // context; the scheduler sleep rule follows the same switch. Call
    // before Start, or between supervisor passes.
    void ConfigureSimulationLod(const LodConfig& config);

    // Applies a (validated, clamped) continuous load field configuration.
    // The world bounds are runtime-owned (terrain extent), never operator
    // config. Rebinds every zone's local load-bin rectangle, so call before
    // Start or between supervisor passes (never with a tick in flight).
    void ConfigureLoadField(const LoadFieldConfig& config);

    // Applies a (validated, clamped) replication/AOI configuration (phase
    // 5B): dirty transform replication, AOI top-k cap reduction, refresh
    // period. Call before Start or between supervisor passes.
    void ConfigureReplication(const ReplicationConfig& config);

    // Load field self-consistency audit in the supervisor's quiescent window
    // (same pattern as RequestValidation): validates dimensions, finite/
    // non-negative channels, the predicted==slow seam, raw totals vs cells,
    // normalization bounds and L1 block sums.
    void RequestLoadFieldValidation();
    bool TryTakeLoadFieldValidationResult(std::string& out_result);

    // Phase 5A exact ghost equivalence audit (publisher fidelity + reconcile
    // diff) in the same quiescent window. Shadow/debug only: it scans all
    // zones' authority, so never run it during a performance measurement.
    void RequestGhostValidation();
    bool TryTakeGhostValidationResult(std::string& out_result);
    // Auto-repair a detected inconsistency (ForceFullGhostReconcile on the
    // affected topology): enabled by default, disabled by the benchmark for
    // pure equivalence measurement.
    void SetGhostAutoRepair(bool enabled) noexcept
    {
        ghost_auto_repair_.store(enabled, std::memory_order_relaxed);
    }
    struct GhostValidationStats {
        std::uint64_t runs = 0;
        std::uint64_t failures = 0;
        std::uint64_t repairs = 0;
    };
    GhostValidationStats GhostValidationSnapshot() const noexcept
    {
        GhostValidationStats stats;
        stats.runs = ghost_validation_runs_.load(std::memory_order_relaxed);
        stats.failures = ghost_validation_failures_.load(std::memory_order_relaxed);
        stats.repairs = ghost_repairs_.load(std::memory_order_relaxed);
        return stats;
    }

    // Phase 5B replication shadow audit (same quiescent window): exact
    // interest-set equivalence (brute force vs the production interest set)
    // plus the recipient coverage invariant (every visible entity's last-sent
    // transform version is not older than its current one). Read-only;
    // mismatches are counted and reported, never silently repaired.
    void RequestReplicationValidation();
    bool TryTakeReplicationValidationResult(std::string& out_result);
    // Phase 5C: retain each tick's canonical shared records so the shadow
    // validator can compare them against authority (debug/bench only; the
    // flag is propagated to every zone, including split children).
    void SetReplicationAudit(bool enabled) noexcept
    {
        replication_audit_.store(enabled, std::memory_order_relaxed);
    }
    struct ReplicationValidationStats {
        std::uint64_t runs = 0;
        std::uint64_t failures = 0;
    };
    ReplicationValidationStats ReplicationValidationSnapshot() const noexcept
    {
        ReplicationValidationStats stats;
        stats.runs = replication_validation_runs_.load(std::memory_order_relaxed);
        stats.failures = replication_validation_failures_.load(std::memory_order_relaxed);
        return stats;
    }

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
    // Member wiring only (no world): both public constructors delegate here.
    struct ConstructMembersOnly {};
    WorldRuntime(boost::asio::io_context& io, RuntimeIdentity identity, ConstructMembersOnly);
    // Shared world bootstrap: geometry, zones, directory and the derived
    // fields (activity + load). File-backed worlds pass their validated spawn
    // points (spawned at once); synthetic worlds pass none and register
    // their own before an explicit bulk spawn.
    // Streaming setup of a file-backed world (MAP-3).
    struct StreamingSetup {
        std::shared_ptr<const mx::map::ChunkSource> source;
        std::vector<std::uint32_t> startup_chunks;
        TerrainStreamingConfig config;
    };
    void InitializeWorld(TerrainService terrain,
                         mx::map::WorldLogic logic,
                         const std::string& mob_types_config,
                         std::optional<std::vector<MobSpawnPoint>> package_spawn_points,
                         const PartitionLayout& layout,
                         std::optional<StreamingSetup> streaming = std::nullopt);
    // Streaming: spawns the configured mobs in batches of spawn circles whose
    // chunks fit the terrain budget (blocking loads, before Start).
    std::size_t SpawnConfiguredMobsStreaming();
    // Supervisor: zone demand -> streamer, then one streamer control step.
    void PumpTerrain();
    void Enqueue(std::function<void()> command);
    void Run();
    void TickZone(std::size_t zone_index);
    ZoneTickContext BuildZoneTickContext();
    void DrainGlobalCommands();
    // Slow topology control plane (~Hz): load monitor update + at most the
    // due split/merge transactions. Runs only when no zone tick is in
    // flight, so partition mutation never races worker threads.
    void ExecutePartitionControl();
    struct PendingSnapshot {
        std::function<void(const SnapshotContext&)> task;
        std::chrono::steady_clock::time_point requested_at;
    };
    // MAP-0 snapshots: type-erased request queue + the supervisor-side
    // capture (only in a quiescent window; the final one at shutdown).
    void EnqueueSnapshot(std::function<void(const SnapshotContext&)> task);
    void ServeSnapshots();
    // Runs a batch of collectors against one capture point (supervisor, or
    // the requester while the runtime is not running). Caller guarantees
    // quiescence.
    void RunSnapshotBatch(std::vector<PendingSnapshot>& batch, std::uint64_t epoch);
    // H9: proves retired zone slots unreferenced (quiescent window, once per
    // world tick) so the next split/merge reuses them; publishes the stats.
    void ReclaimRetiredZones();
    // One full split transaction: Plan -> Create(staged) -> Transfer ->
    // Validate -> Commit, with rollback + AbortSplit on any failure (§3-4).
    // `forced` bypasses load predicates (test seams); `center` is the
    // adaptive scorer's cut point (null = geometric midpoint). The machinery
    // is otherwise identical. Returns true on commit.
    bool RunSplitTransaction(ZoneId zone_id, bool forced, const SplitCenter* center = nullptr);
    // One full merge transaction: Plan -> CreateTarget(staged) -> Transfer
    // -> Validate -> CommitTreeCollapse, with rollback + AbortMerge (§5).
    bool RunMergeTransaction(ZoneId parent_node_id, bool forced);
    // Supervisor-side bodies behind PostForceSplit/PostForceMerge.
    void ExecuteForcedSplit(ZoneId zone_id);
    void ExecuteForcedMerge(ZoneId parent_node_id);
    // True when the migration queue holds a request touching the zone
    // (source or target): transactions never start under a racing migration.
    bool ZoneHasPendingMigration(ZoneId zone_id) const;
    // Read-only scoring helper: builds the scorer input for a zone from live
    // (quiescent or atomic) state and runs the scorer. Never mutates.
    SplitRecommendation ScorePartitionWith(ZoneId zone_id,
                                           const std::shared_ptr<const LoadGrid>& field,
                                           const std::shared_ptr<const ActivityGrid>& activity,
                                           std::chrono::steady_clock::time_point now) const;
    MergeRecommendation ScoreMergeWith(ZoneId parent_id,
                                       const std::shared_ptr<const LoadGrid>& field,
                                       const std::shared_ptr<const ActivityGrid>& activity,
                                       std::chrono::steady_clock::time_point now) const;
    // Partition-tree node lookup (leaf or internal); null when absent.
    const ZonePartition* FindZoneNode(ZoneId zone_id) const;
    // Appends one decision record (bounded) and logs it as one line when the
    // decision log is enabled; why-not lines are rate-limited per zone.
    void RecordPartitionDecision(PartitionDecisionRecord record, bool executed, bool why_not);
    // Merge-specific record builders (structured breakdown preserved).
    void RecordMergeDecision(const MergeRecommendation& recommendation,
                             PartitionNoopReason reason,
                             const char* detail,
                             bool executed,
                             std::chrono::steady_clock::time_point now);
    void RecordMergeGateNoop(const MergeGroupSnapshot& group,
                             std::chrono::steady_clock::time_point now);
    // Stability: counts a reversal when a mutation commits while the
    // opposite-direction mutation on the same node is inside the oscillation
    // window (diagnostic; directional cooldowns are the hard guards).
    void NoteOscillationIfAny(const ZonePartition* node,
                              bool split_committed,
                              std::chrono::steady_clock::time_point now);
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
    std::atomic<std::thread::id> sim_thread_id_{};
    // MAP-0 snapshot requests (any thread enqueues; the supervisor serves
    // them in a quiescent window, or at shutdown before the world is torn
    // down, so no future is left hanging).
    std::mutex snapshot_mutex_;
    std::vector<PendingSnapshot> pending_snapshots_;
    bool snapshot_serving_ = false; // guarded by snapshot_mutex_; true while Run() owns the world
    std::atomic<bool> snapshots_pending_{false};
    std::uint64_t snapshot_epoch_ = 0;
    std::atomic<std::uint64_t> snapshot_captures_{0};
    std::atomic<std::uint64_t> snapshot_requests_{0};
    std::atomic<std::uint64_t> snapshot_max_wait_us_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> commands_;
    // MAP-3: an I/O worker finished a chunk load (guarded by mutex_, part of
    // the supervisor's wait predicate -- a bare notify would be swallowed by
    // the predicate and the completion would wait for the idle timeout).
    bool terrain_completion_pending_ = false;

    ZoneManager zones_;
    ZoneScheduler scheduler_;
    ZoneWorkerPool workers_;

    RuntimeIdentity identity_;
    WorldDirectory directory_;
    WorldMessageRouter router_;
    MigrationTransport migration_transport_;

    TerrainService terrain_;
    // MAP-3: chunk streaming (null for eager / flat terrain). Declared after
    // terrain_: it mutates terrain_'s published slots and is destroyed first.
    std::unique_ptr<TerrainStreamer> streamer_;
    std::vector<TerrainDemand> terrain_demand_scratch_;
    std::chrono::steady_clock::time_point terrain_drain_started_{};
    std::atomic<std::uint64_t> terrain_safepoints_{0}, terrain_drain_wait_us_{0};
    std::atomic<std::uint64_t> terrain_commands_requested_{0}, terrain_ingress_rejected_{0};
    std::atomic<std::uint64_t> supervisor_cpu_us_{0}, terrain_drain_max_us_{0};
    std::atomic<std::uint32_t> terrain_commands_pending_{0};
    mutable std::mutex terrain_stats_mutex_;
    TerrainQueryCounters terrain_queries_;
    NavigationService navigation_;
    std::atomic<std::uint64_t> next_navigation_id_{0};
    static constexpr std::uint32_t kNavigationExpansionsPerPass = 4000;
    mx::map::WorldLogic world_logic_;
    OwnerMap owners_by_session_;
    MigrationQueue migration_queue_;

    SpawnCoordinator spawn_;
    MigrationCoordinator migration_;
    InputRouter inputs_;
    InputRouter::AttackHandler attack_handler_;

    ZoneLoadMonitor load_monitor_;
    // Slow control-plane cadence (§42): topology decisions at ~1 Hz while
    // simulation runs at 20 Hz.
    std::chrono::steady_clock::time_point last_partition_control_{};
    std::uint32_t last_reclaim_tick_ = UINT32_MAX;
    mutable std::mutex reclaim_stats_mutex_;
    ZoneManager::ReclaimStats reclaim_stats_{}; // published copy (any thread)
    // World-space activity field (first Adaptive Simulation Fabric
    // foundation). Rebuilt ~1Hz on the supervisor from per-zone published
    // player sources; consumed via immutable snapshots by zone ticks
    // (LOD eval), the scheduler (sleep/wake) and validators/bench.
    SpatialActivityField activity_field_;
    std::chrono::steady_clock::time_point last_activity_build_{};
    // Continuous multi-channel load field (Adaptive Simulation Fabric phase 1).
    // World-space derived work map, aggregated ~1Hz on the supervisor from
    // per-zone published load bins; consumed via immutable snapshots. The
    // partition tree is a future consumer, never an owner (§25-26).
    ContinuousLoadField load_field_;
    std::chrono::steady_clock::time_point last_load_field_build_{};
    // Adaptive partition scoring (read-only). The monitor observes; the
    // scorer recommends; the existing transactional executor mutates.
    PartitionScorer scorer_;
    mutable std::mutex decision_mutex_;
    std::vector<PartitionDecisionRecord> decisions_;
    std::unordered_map<ZoneId, std::chrono::steady_clock::time_point> why_not_last_logged_;
    PartitionMetrics partition_metrics_;
    PartitionConfig effective_partition_config_;
    LodConfig effective_lod_config_;
    LoadFieldConfig effective_load_field_config_;
    ReplicationConfig effective_replication_config_;

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

    std::atomic<bool> load_field_validation_requested_{false};
    mutable std::mutex load_field_validation_mutex_;
    std::string load_field_validation_result_;
    bool load_field_validation_ready_ = false;

    // Phase 5A ghost equivalence validation + repair seam.
    void RepairGhosts();
    std::atomic<bool> ghost_validation_requested_{false};
    mutable std::mutex ghost_validation_mutex_;
    std::string ghost_validation_result_;
    bool ghost_validation_ready_ = false;
    std::atomic<std::uint64_t> ghost_validation_runs_{0};
    std::atomic<std::uint64_t> ghost_validation_failures_{0};
    std::atomic<std::uint64_t> ghost_repairs_{0};
    std::atomic<bool> ghost_auto_repair_{true};

    // Phase 5B replication shadow validation (read-only; no repair path --
    // mismatches must stay visible, see phase 5B §89).
    std::atomic<bool> replication_validation_requested_{false};
    mutable std::mutex replication_validation_mutex_;
    std::string replication_validation_result_;
    bool replication_validation_ready_ = false;
    std::atomic<std::uint64_t> replication_validation_runs_{0};
    std::atomic<std::uint64_t> replication_validation_failures_{0};
    std::atomic<bool> replication_audit_{false};

    // Phase 7 scheduler audit.
    std::size_t requested_workers_ = 0;
    // Diagnostics log/exchange cadence (the *_since_diag windows). Decisions
    // never read those windows (H5); the knob exists to prove it.
    std::chrono::milliseconds diagnostics_interval_{1000};
    std::atomic<std::uint64_t> busy_phase_micros_{0};
    std::atomic<std::uint64_t> idle_phase_micros_{0};
    std::chrono::steady_clock::time_point last_phase_sample_{};
};

} // namespace gs::game
