#include "WorldRuntime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "common/Logging.h"

#include "components/Tags.h"
#include "components/TransformComponents.h"
#include "distributed/RuntimeIds.h"
#include "migration/EntityTransfer.h"
#include "partition/ZonePartition.h"
#include "replication/NetworkSend.h"
#include "systems/CombatSystem.h"
#include "visibility/GhostSystem.h"
#include "zone/ZoneOwnership.h"
#include "WorldConstants.h"

namespace gs::game {
namespace {

mx::map::WorldLogic LoadWorldLogicFromMapRoot(const std::string& map_root)
{
    const std::filesystem::path root(map_root);
    auto logic = mx::map::LoadWorldLogic(
        [&root](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            std::filesystem::path normalized(path);
            std::ifstream file(root / normalized.relative_path(), std::ios::binary | std::ios::ate);
            if (!file) {
                return std::nullopt;
            }
            const auto end = file.tellg();
            if (end < 0) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
            file.seekg(0);
            if (!bytes.empty()) {
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!file) {
                    return std::nullopt;
                }
            }
            return bytes;
        },
        ".");
    if (logic) {
        LOG_INFO("Game sim loaded worldlogic: zones={} spawns={} warps={}",
                 logic->zones.size(),
                 logic->spawns.size(),
                 logic->warps.size());
        return std::move(*logic);
    }
    LOG_WARN("Game sim worldlogic load failed from {}; using fallback single zone", map_root);
    return mx::map::WorldLogic{};
}

} // namespace

WorldRuntime::WorldRuntime(boost::asio::io_context& io, RuntimeIdentity identity)
    : io_(io)
    , workers_([this](std::size_t zone_index) {
        TickZone(zone_index);
    })
    , identity_(identity)
    , directory_(identity)
    , router_(zones_, directory_)
    , spawn_(io_,
             zones_,
             terrain_,
             world_logic_,
             owners_by_session_,
             router_,
             [this] {
                 cv_.notify_one();
             },
             [this](std::shared_ptr<gs::network::Session> session, std::vector<std::uint8_t> payload) {
                 SendToSession(io_, session, std::move(payload));
             },
             identity_,
             directory_)
    , migration_(zones_, terrain_, owners_by_session_, migration_queue_, directory_,
                 migration_transport_, identity_)
    , inputs_(router_, [this] {
        cv_.notify_one();
    })
{
    const std::string map_root = IXTREEME_DEFAULT_MAP_ROOT;
    terrain_ = TerrainService::LoadFromMapRoot(map_root);
    world_logic_ = LoadWorldLogicFromMapRoot(map_root);

    zones_.BuildFromWorldLogic(world_logic_, terrain_.WorldExtentMeters());
    directory_.RebuildFromManager(zones_);
    spawn_.Initialize(map_root, IXTREEME_DEFAULT_MOB_TYPES_CONFIG);
}

WorldRuntime::~WorldRuntime()
{
    Stop();
}

void WorldRuntime::Start()
{
    if (thread_.joinable()) {
        return;
    }

    stopping_ = false;
    thread_ = std::thread([this] {
        Run();
    });
}

void WorldRuntime::Stop()
{
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    workers_.Stop();
}

void WorldRuntime::PostSpawn(std::shared_ptr<gs::network::Session> session,
                             gs::db::Character character,
                             std::optional<DebugSpawnOverride> debug_spawn)
{
    Enqueue([this,
             session = std::move(session),
             character = std::move(character),
             debug_spawn]() mutable {
        spawn_.Spawn(std::move(session), std::move(character), debug_spawn, world_tick_.load());
    });
}

void WorldRuntime::PostDespawn(gs::common::SessionId session_id)
{
    Enqueue([this, session_id] {
        spawn_.Despawn(session_id);
    });
}

void WorldRuntime::PostMoveInput(gs::common::SessionId session_id,
                                 std::uint32_t sequence,
                                 float dir_angle,
                                 MoveState state)
{
    inputs_.PostMoveInput(session_id, sequence, dir_angle, state);
}

void WorldRuntime::PostAttackTarget(gs::common::SessionId session_id, std::uint32_t target_net_id)
{
    inputs_.PostAttackTarget(session_id, target_net_id);
}

void WorldRuntime::AddMobSpawnPoint(const MobSpawnPoint& point)
{
    spawn_.AddSpawnPoint(point);
}

void WorldRuntime::RequestMobSpawn(std::size_t spawn_point_index)
{
    spawn_.ScheduleRespawn(spawn_point_index, 0.0f);
}

void WorldRuntime::Enqueue(std::function<void()> command)
{
    {
        std::lock_guard lock(mutex_);
        commands_.push(std::move(command));
    }
    cv_.notify_one();
}

ZoneTickContext WorldRuntime::BuildZoneTickContext()
{
    ZoneTickContext ctx{terrain_,
                        world_logic_,
                        spawn_.MobTypes(),
                        zones_,
                        &migration_queue_,
                        world_tick_.load(std::memory_order_relaxed),
                        &effective_lod_config_,
                        [this](std::shared_ptr<gs::network::Session> session, std::vector<std::uint8_t> payload) {
                            SendToSession(io_, session, std::move(payload));
                        },
                        [this](std::size_t spawn_point_index, float delay_sec) {
                            spawn_.ScheduleRespawn(spawn_point_index, delay_sec);
                        }};
    return ctx;
}

void WorldRuntime::TickZone(std::size_t zone_index)
{
    if (zone_index >= zones_.ZoneCount()) {
        return;
    }
    auto ctx = BuildZoneTickContext();
    zones_.GetZone(zone_index).Tick(kTickDtSeconds, ctx);
}

void WorldRuntime::Run()
{
    sim_thread_id_ = std::this_thread::get_id();

    workers_.Start(zones_.ZoneCount());

    LOG_INFO("Game sim supervisor started: zones={} workers={} aoi_radius={} aoi_cap={}",
             zones_.ZoneCount(),
             workers_.WorkerCount(),
             kAoiRadiusMeters,
             kAoiEntityCap);
    auto next_world_tick = std::chrono::steady_clock::now() + kTickDt;
    auto next_diagnostics = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (!stopping_) {
        const auto supervisor_start = std::chrono::steady_clock::now();
        DrainGlobalCommands();
        migration_.ProcessMigrations(world_tick_.load(std::memory_order_relaxed));
        spawn_.ProcessRespawns(kTickDtSeconds);
        inputs_.DrainMoves(owners_by_session_);
        inputs_.DrainAttacks(owners_by_session_,
                             [this](Zone& zone,
                                     gs::common::SessionId attacker,
                                     std::uint32_t target_net_id) {
                                 auto ctx = BuildZoneTickContext();
                                 const auto result =
                                     CombatSystem::ProcessAttack(zone, attacker, target_net_id, ctx);
                                 if (result.attacked) {
                                     attacks_since_diag_.fetch_add(1, std::memory_order_relaxed);
                                     attacks_total_.fetch_add(1, std::memory_order_relaxed);
                                 }
                                 if (result.killed) {
                                     deaths_total_.fetch_add(1, std::memory_order_relaxed);
                                 }
                             });
        scheduler_.ScheduleOnce(zones_, workers_, std::chrono::steady_clock::now());
        ExecutePartitionControl();
        const auto supervisor_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 supervisor_start)
                .count());
        supervisor_micros_since_diag_ += supervisor_micros;
        supervisor_micros_total_.fetch_add(supervisor_micros, std::memory_order_relaxed);
        supervisor_samples_.fetch_add(1, std::memory_order_relaxed);

        // Operational audit window: when no zone tick is in progress and no
        // new tick will start before ScheduleOnce already ran above... note
        // new tasks were just enqueued, but their tick_in_progress flags are
        // already set, so AnyTickInProgress() covers them. All workers are
        // parked here, and the supervisor itself is between mutations: zone
        // state is stable to read.
        if (validation_requested_.load(std::memory_order_relaxed)) {
            if (!zones_.AnyTickInProgress()) {
                validation_requested_.store(false, std::memory_order_relaxed);
                std::string error;
                const bool ok = ValidateWorldConsistency(zones_, owners_by_session_, migration_queue_,
                                                         directory_, error);
                std::lock_guard lock(validation_mutex_);
                validation_result_ = ok ? std::string("OK") : "FAIL: " + error;
                validation_ready_ = true;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_world_tick) {
            do {
                world_tick_.fetch_add(1, std::memory_order_relaxed);
                next_world_tick += kTickDt;
            } while (now >= next_world_tick);
        }

        if (now >= next_diagnostics) {
            std::uint64_t total_ticks = 0;
            std::uint64_t total_records = 0;
            std::uint64_t total_empty_skips = 0;
            std::uint64_t total_migrations = 0;
            std::uint64_t total_tick_micros = 0;
            std::uint64_t total_aoi_queries = 0;
            std::uint64_t total_dirty_xf = 0;
            std::uint64_t total_tier_near = 0;
            std::uint64_t total_tier_mid = 0;
            std::uint64_t total_tier_far = 0;
            std::uint64_t total_gameplay_micros = 0;
            std::uint64_t total_ghost_micros = 0;
            std::uint64_t total_repl_micros = 0;
            std::uint64_t total_lod_ai = 0;
            std::uint64_t total_lod_mv = 0;
            std::uint64_t total_lod_prom = 0;
            std::uint64_t total_lod_dem = 0;
            std::uint64_t total_lod_wake = 0;
            std::uint64_t total_lod_eval_us = 0;
            std::uint64_t lod_full = 0;
            std::uint64_t lod_reduced = 0;
            std::uint64_t lod_low = 0;
            std::uint64_t lod_dormant = 0;
            const auto attacks_per_sec = attacks_since_diag_.exchange(0);
            std::size_t active_zones = 0;
            std::size_t sleeping_zones = 0;
            std::size_t active_sessions = 0;
            std::size_t active_ghosts = 0;
            for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
                auto& zone = zones_.GetZone(i);
                const auto player_count = zone.Diagnostics().player_count.load();
                const auto mob_count = zone.Diagnostics().mob_count.load();
                active_sessions += player_count;
                active_ghosts += zone.Diagnostics().ghost_count.load();
                if (player_count > 0 || mob_count > 0) {
                    ++active_zones;
                }
                if (zone.Activity() == ZoneActivity::Sleeping) {
                    ++sleeping_zones;
                }
                lod_full += zone.Diagnostics().lod_full.load(std::memory_order_relaxed);
                lod_reduced += zone.Diagnostics().lod_reduced.load(std::memory_order_relaxed);
                lod_low += zone.Diagnostics().lod_low.load(std::memory_order_relaxed);
                lod_dormant += zone.Diagnostics().lod_dormant.load(std::memory_order_relaxed);
                total_ticks += zone.Diagnostics().ticks_since_diag.exchange(0);
                total_records += zone.Diagnostics().transform_records_since_diag.exchange(0);
                total_empty_skips += zone.Diagnostics().empty_skips_since_diag.exchange(0);
                total_migrations += zone.Diagnostics().migrations_since_diag.exchange(0);
                total_tick_micros += zone.Diagnostics().tick_micros_since_diag.exchange(0);
                total_aoi_queries += zone.Diagnostics().aoi_queries_since_diag.exchange(0);
                total_dirty_xf += zone.Diagnostics().transform_dirty_since_diag.exchange(0);
                total_tier_near += zone.Diagnostics().tier_near_since_diag.exchange(0);
                total_tier_mid += zone.Diagnostics().tier_mid_since_diag.exchange(0);
                total_tier_far += zone.Diagnostics().tier_far_since_diag.exchange(0);
                total_gameplay_micros += zone.Diagnostics().gameplay_micros_since_diag.exchange(0);
                total_ghost_micros += zone.Diagnostics().ghost_micros_since_diag.exchange(0);
                total_repl_micros += zone.Diagnostics().replication_micros_since_diag.exchange(0);
                total_lod_ai += zone.Diagnostics().lod_ai_updates_since_diag.exchange(0);
                total_lod_mv += zone.Diagnostics().lod_move_updates_since_diag.exchange(0);
                total_lod_prom += zone.Diagnostics().lod_promotions_since_diag.exchange(0);
                total_lod_dem += zone.Diagnostics().lod_demotions_since_diag.exchange(0);
                total_lod_wake += zone.Diagnostics().lod_wakes_since_diag.exchange(0);
                total_lod_eval_us += zone.Diagnostics().lod_eval_us_since_diag.exchange(0);
            }
            lod_ai_total_.fetch_add(total_lod_ai, std::memory_order_relaxed);
            lod_mv_total_.fetch_add(total_lod_mv, std::memory_order_relaxed);
            lod_prom_total_.fetch_add(total_lod_prom, std::memory_order_relaxed);
            lod_dem_total_.fetch_add(total_lod_dem, std::memory_order_relaxed);
            lod_wake_total_.fetch_add(total_lod_wake, std::memory_order_relaxed);
            lod_eval_us_total_.fetch_add(total_lod_eval_us, std::memory_order_relaxed);
            std::size_t active_mobs = 0;
            std::size_t wandering_mobs = 0;
            std::size_t idle_mobs = 0;
            for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
                auto& zone = zones_.GetZone(i);
                active_mobs += zone.Diagnostics().mob_count.load();
                wandering_mobs += zone.Diagnostics().wandering_mob_count.load();
                idle_mobs += zone.Diagnostics().idle_mob_count.load();
            }
            const double avg_tick_ms =
                total_ticks > 0 ? static_cast<double>(total_tick_micros) / total_ticks / 1000.0 : 0.0;
            const double avg_supervisor_ms =
                static_cast<double>(supervisor_micros_since_diag_) / 1000.0;
            supervisor_micros_since_diag_ = 0;
            const auto worker_util = workers_.GetUtilization();
            const double worker_busy_pct =
                workers_.WorkerCount() > 0
                    ? static_cast<double>(worker_util.busy_micros) / 1'000'000.0 /
                          static_cast<double>(workers_.WorkerCount()) * 100.0
                    : 0.0;
             const auto routes = router_.MetricsSnapshot();
             const auto mig_metrics = migration_.MetricsSnapshot();
             const auto part_metrics = partition_metrics_.TakeSnapshot();
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} sleeping_zones={} active_sessions={} active_mobs={} wandering_mobs={} idle_mobs={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} attacks_per_sec={} deaths_total={} respawns_pending={} respawns_total={} migrations={} mig_pending={} mig_quarantined={} mig_detail=[c={} stale={} dup={} retry={} fail={}] routes=[local={} remu={} unav={} drain={} miss={}] workers={} worker_busy_pct={:.1f}                      avg_zone_tick_ms={:.3f} aoi_queries={} dirty_xf={} tiers=[{}/{}/{}] stage_us=[gameplay={} ghost={} repl={}] avg_supervisor_ms={:.3f} partition=[s_att={} s_ok={} s_ab={} m_att={} m_ok={} m_ab={} rej={}] lod=[{}/{}/{}/{} ai={} mv={} prom={} dem={} wake={} eval_us={}]",
                     world_tick_.load(),
                     zones_.ZoneCount(),
                     active_zones,
                     sleeping_zones,
                     active_sessions,
                     active_mobs,
                     wandering_mobs,
                     idle_mobs,
                     active_ghosts,
                     total_ticks,
                     total_empty_skips,
                     total_records,
                     attacks_per_sec,
                     deaths_total_.load(),
                     spawn_.RespawnsPending(),
                     spawn_.RespawnsTotal(),
                     total_migrations,
                     migration_queue_.PendingCount(),
                     migration_.QuarantinedCount(),
                     mig_metrics.committed,
                     mig_metrics.dropped_stale,
                     mig_metrics.duplicates,
                     mig_metrics.retries,
                     mig_metrics.failures,
                     routes.local_delivered,
                     routes.remote_emulated,
                     routes.unavailable,
                     routes.draining,
                     routes.directory_miss,
                     workers_.WorkerCount(),
                     worker_busy_pct,
                     avg_tick_ms,
                     total_aoi_queries,
                     total_dirty_xf,
                     total_tier_near,
                     total_tier_mid,
                     total_tier_far,
                     total_gameplay_micros,
                     total_ghost_micros,
                     total_repl_micros,
                     avg_supervisor_ms,
                     part_metrics.split_attempts,
                     part_metrics.split_commits,
                     part_metrics.split_aborts,
                     part_metrics.merge_attempts,
                     part_metrics.merge_commits,
                     part_metrics.merge_aborts,
                     part_metrics.retire_rejected_nonempty,
                     lod_full,
                     lod_reduced,
                     lod_low,
                     lod_dormant,
                     total_lod_ai,
                     total_lod_mv,
                     total_lod_prom,
                     total_lod_dem,
                     total_lod_wake,
                     total_lod_eval_us);
            do {
                next_diagnostics += std::chrono::seconds(1);
            } while (now >= next_diagnostics);
        }

        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this] {
            return stopping_.load() || !commands_.empty();
        });
    }

    DrainGlobalCommands();
    while (zones_.AnyTickInProgress()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    zones_.Clear();
    owners_by_session_.clear();
    spawn_.ClearRespawns();
    LOG_INFO("Game sim supervisor stopped");
}

bool WorldRuntime::ValidateConsistency(std::string& out_error)
{
    // Debug/test only: the caller must guarantee no zone tick or supervisor
    // mutation is running concurrently (e.g. call between Run iterations in
    // a test harness, or after Stop).
    return ValidateWorldConsistency(zones_, owners_by_session_, migration_queue_, directory_,
                                    out_error);
}

void WorldRuntime::RequestValidation()
{
    validation_requested_.store(true, std::memory_order_relaxed);
}

bool WorldRuntime::TryTakeValidationResult(std::string& out_result)
{
    std::lock_guard lock(validation_mutex_);
    if (!validation_ready_) {
        return false;
    }
    out_result = std::move(validation_result_);
    validation_result_.clear();
    validation_ready_ = false;
    return true;
}

double WorldRuntime::SupervisorAvgMs() const
{
    const auto samples = supervisor_samples_.load(std::memory_order_relaxed);
    return samples > 0 ? static_cast<double>(supervisor_micros_total_.load(std::memory_order_relaxed)) /
                             samples / 1000.0
                       : 0.0;
}

ZoneWorkerPool::Utilization WorldRuntime::WorkerUtilization() const
{
    return workers_.GetUtilization();
}

std::size_t WorldRuntime::MigrationQuarantined() const
{
    return migration_.QuarantinedCount();
}

MigrationId WorldRuntime::LastCommittedMigration() const
{
    return migration_.LastCommittedId();
}

MigrationMetrics::Snapshot WorldRuntime::MigrationMetrics() const
{
    return migration_.MetricsSnapshot();
}

void WorldRuntime::EmulateDistribution(std::uint32_t logical_processes)
{
    if (logical_processes < 2) {
        return;
    }
    router_.SetRemoteMode(WorldMessageRouter::RemoteMode::EmulatedLoopback);
    migration_transport_.SetRemoteMode(MigrationTransport::RemoteMode::EmulatedLoopback);
    for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
        const ZoneId zone_id = zones_.GetZone(i).Id();
        // Zone 0 stays home; the rest stripe across logical processes 2..K.
        // Node stays 1: this emulates multi-PROCESS, single-node sharding.
        const std::uint32_t process =
            (i == 0) ? identity_.process.value : static_cast<std::uint32_t>((i % logical_processes) + 1);
        const ProcessId pid{process == 0 ? 1 : process};
        directory_.NoteRemoteAlive(identity_.node, pid);
        directory_.SetAssignment(zone_id, ZoneLocation{identity_.node, pid, zone_id});
    }
}

ProcessLoadSnapshot WorldRuntime::CollectProcessLoad() const
{
    ProcessLoadSnapshot snapshot;
    snapshot.identity = identity_;
    snapshot.world_tick = world_tick_.load(std::memory_order_relaxed);
    snapshot.zone_count = zones_.ZoneCount();
    std::uint64_t tick_total = 0;
    std::uint64_t tick_count = 0;
    for (std::size_t i = 0; i < zones_.ZoneCount(); ++i) {
        const auto& zone = zones_.GetZone(i);
        const auto& diag = zone.Diagnostics();
        const auto players = diag.player_count.load(std::memory_order_relaxed);
        const auto mobs = diag.mob_count.load(std::memory_order_relaxed);
        snapshot.players += players;
        snapshot.mobs += mobs;
        snapshot.ghosts += diag.ghost_count.load(std::memory_order_relaxed);
        if (players > 0 || mobs > 0) {
            ++snapshot.active_zones;
        }
        if (zone.Activity() == ZoneActivity::Sleeping) {
            ++snapshot.sleeping_zones;
        }
        // Non-destructive reads: the periodic diag owns the exchange().
        const std::uint64_t ticks = diag.ticks_since_diag.load(std::memory_order_relaxed);
        tick_total += diag.tick_micros_since_diag.load(std::memory_order_relaxed);
        tick_count += ticks;
        snapshot.repl_records += diag.transform_records_since_diag.load(std::memory_order_relaxed);
        snapshot.migrations += diag.migrations_since_diag.load(std::memory_order_relaxed);
    }
    snapshot.avg_zone_tick_ms =
        tick_count > 0 ? static_cast<double>(tick_total) / tick_count / 1000.0 : 0.0;
    const auto worker_util = workers_.GetUtilization();
    snapshot.worker_tasks = worker_util.tasks_completed;
    snapshot.worker_busy_us = worker_util.busy_micros;
    snapshot.supervisor_avg_ms = SupervisorAvgMs();
    const auto routes = router_.MetricsSnapshot();
    snapshot.routes_local = routes.local_delivered;
    snapshot.routes_remote_emulated = routes.remote_emulated;
    snapshot.routes_unavailable = routes.unavailable;
    snapshot.routes_draining = routes.draining;
    CollectZoneLoadMetrics(zones_, snapshot.zones);
    return snapshot;
}

namespace {

// Rolls back a partially built transfer destination (§7-8). Armed until the
// transfer commits; the destructor never throws, so a failing rollback step
// cannot mask the original error (nor terminate the supervisor).
struct DestinationRollback {
    Zone* target = nullptr;
    std::uint32_t net_id = 0;
    Position position{};
    flecs::entity entity{};
    bool applied = false;
    bool indexed = false;
    bool gridded = false;
    bool bound = false;
    bool rng_moved = false;
    bool committed = false;

    ~DestinationRollback() noexcept
    {
        if (committed || target == nullptr || !applied) {
            return;
        }
        try {
            if (bound) {
                target->ErasePlayerBinding(net_id);
            }
            if (rng_moved) {
                target->EraseMobRng(net_id);
            }
            if (gridded) {
                target->Grid().Remove(net_id, position);
            }
            if (indexed) {
                target->UnindexEntity(net_id);
            }
            if (entity.is_valid()) {
                entity.destruct();
            }
        } catch (...) {
            // Rollback is last-resort cleanup: never throw out of it.
        }
    }

    void commit() noexcept
    {
        committed = true;
    }
};

// Deterministic failure injection (§17-18): consume one token if armed.
// Single consumer (supervisor) + single setter (bench between passes).
bool ConsumeTestFailure(std::atomic<int>& counter)
{
    int remaining = counter.load(std::memory_order_relaxed);
    while (remaining > 0) {
        if (counter.compare_exchange_weak(remaining,
                                          remaining - 1,
                                          std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point from,
                        std::chrono::steady_clock::time_point to)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

} // namespace

void WorldRuntime::ExecutePartitionControl()
{
    // Topology mutation must never race worker zone ticks: bail unless the
    // world is quiescent. Ticks are short; the 1 Hz control cadence still
    // gets plenty of windows.
    if (zones_.AnyTickInProgress()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_partition_control_ < std::chrono::seconds(1)) {
        return;
    }
    last_partition_control_ = now;

    load_monitor_.Update(zones_, scheduler_, now);

    // Max one topology mutation per control cycle: staged reshaping, never
    // a multi-split spike in one frame.
    for (const ZoneId zone_id : load_monitor_.SplitCandidates()) {
        RunSplitTransaction(zone_id, false);
        break;
    }
    for (const ZoneId parent_id : load_monitor_.MergeCandidates()) {
        RunMergeTransaction(parent_id, false);
        break;
    }
}

bool WorldRuntime::ZoneHasPendingMigration(ZoneId zone_id) const
{
    for (const auto& request : migration_queue_.RequestSnapshot()) {
        if (request.source_zone_id == zone_id || request.target_zone_id == zone_id) {
            return true;
        }
    }
    return false;
}

void WorldRuntime::ExecuteForcedSplit(ZoneId zone_id)
{
    if (zones_.AnyTickInProgress()) {
        LOG_WARN("partition: forced split deferred (tick in flight), re-queued");
        PostForceSplit(zone_id);
        return;
    }
    RunSplitTransaction(zone_id, true);
}

void WorldRuntime::ExecuteForcedMerge(ZoneId parent_node_id)
{
    if (zones_.AnyTickInProgress()) {
        LOG_WARN("partition: forced merge deferred (tick in flight), re-queued");
        PostForceMerge(parent_node_id);
        return;
    }
    RunMergeTransaction(parent_node_id, true);
}

bool WorldRuntime::RunSplitTransaction(ZoneId zone_id, bool forced)
{
    const auto t_plan0 = std::chrono::steady_clock::now();
    ZoneManager::SplitPlan plan;
    if (!zones_.PlanSplit(zone_id, plan)) {
        return false; // routine skip, not an abort
    }
    if (ZoneHasPendingMigration(zone_id)) {
        LOG_INFO("partition: split deferred zone={} (racing migration, retry next cycle)", zone_id);
        return false;
    }
    partition_metrics_.split_plan_us.fetch_add(ElapsedUs(t_plan0, std::chrono::steady_clock::now()),
                                               std::memory_order_relaxed);
    partition_metrics_.split_attempts.fetch_add(1, std::memory_order_relaxed);

    std::vector<ZoneId> children;
    if (!zones_.CreateStagedSplit(plan, children)) {
        partition_metrics_.split_aborts.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("partition: split ABORTED zone={} (staging failed, parent restored)", zone_id);
        return false;
    }
    const std::size_t parent_index = plan.parent_index;

    // Per-child locations (same node/process as the parent, own zone id).
    // Published ONLY at commit: nothing routes here prematurely (§11).
    const auto parent_loc = directory_.ResolveZone(zone_id);
    std::unordered_map<ZoneId, ZoneLocation> child_locs;
    for (const ZoneId child_id : children) {
        ZoneLocation child_loc = parent_loc.value_or(LocalZoneLocation(identity_, child_id));
        child_loc.zone = child_id;
        child_locs.emplace(child_id, child_loc);
    }

    struct PlannedMove {
        std::uint32_t net_id = 0;
        std::size_t target_index = 0;
        ZoneId target_id = 0;
    };
    auto abort_split = [&](const char* reason, std::vector<PlannedMove>& moved) -> bool {
        const auto t_rb0 = std::chrono::steady_clock::now();
        std::uint64_t rb_fail = 0;
        ZoneLocation back = parent_loc.value_or(LocalZoneLocation(identity_, zone_id));
        back.zone = zone_id;
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            if (!TransferResident(it->target_index, parent_index, back, it->net_id)) {
                ++rb_fail;
                LOG_ERROR("partition: split rollback failed net_id={} (stays staged; validator "
                          "will flag loudly)",
                          it->net_id);
            }
        }
        zones_.AbortSplit(zone_id, children);
        partition_metrics_.split_aborts.fetch_add(1, std::memory_order_relaxed);
        partition_metrics_.split_rollback_failures.fetch_add(rb_fail, std::memory_order_relaxed);
        partition_metrics_.split_rollback_us.fetch_add(
            ElapsedUs(t_rb0, std::chrono::steady_clock::now()), std::memory_order_relaxed);
        LOG_WARN("partition: split ABORTED zone={} reason={} moved={} rb_fail={} (parent restored)",
                 zone_id,
                 reason,
                 moved.size(),
                 rb_fail);
        return false;
    };

    // ---- Transfer: route every resident to exactly one child (half-open,
    // same rule as FindLeaf). An unroutable entity ABORTS the split (§24):
    // silent misplacement would corrupt authority.
    const auto t_xfer0 = std::chrono::steady_clock::now();
    Zone& parent = zones_.GetZone(parent_index);
    std::vector<PlannedMove> moves;
    bool routable = true;
    for (const auto& [net_id, entity] : parent.Entities()) {
        if (!entity.is_valid() || !entity.has<Position>()) {
            routable = false;
            break;
        }
        const auto pos = entity.get<Position>();
        ZoneId match_id = 0;
        std::size_t match_index = 0;
        bool matched = false;
        for (const ZoneId child_id : children) {
            const std::size_t child_index = zones_.FindIndexById(child_id);
            const auto& b = zones_.GetZone(child_index).Bounds();
            if (pos.x >= b.min_x && pos.x < b.max_x && pos.y >= b.min_y && pos.y < b.max_y) {
                match_id = child_id;
                match_index = child_index;
                matched = true;
                break;
            }
        }
        if (!matched) {
            for (const ZoneId child_id : children) {
                const std::size_t child_index = zones_.FindIndexById(child_id);
                if (zones_.GetZone(child_index).Bounds().Contains(pos.x, pos.y)) {
                    match_id = child_id;
                    match_index = child_index;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            LOG_ERROR("partition: split abort, net_id={} at ({},{}) matches no child of zone {}",
                      net_id,
                      pos.x,
                      pos.y,
                      zone_id);
            routable = false;
            break;
        }
        moves.push_back(PlannedMove{net_id, match_index, match_id});
    }
    if (!routable) {
        std::vector<PlannedMove> empty;
        return abort_split("unroutable-resident", empty);
    }
    // Only COMPLETED moves are ever reversed: aborting over the planned
    // list would "roll back" entities that never left the source (their
    // reverse lookup fails loudly and spuriously).
    std::vector<PlannedMove> completed;
    for (const auto& move : moves) {
        if (!TransferResident(parent_index, move.target_index, child_locs[move.target_id],
                              move.net_id)) {
            partition_metrics_.split_transfer_failures.fetch_add(1, std::memory_order_relaxed);
            return abort_split("transfer-failed", completed);
        }
        completed.push_back(move);
    }
    partition_metrics_.split_transfer_us.fetch_add(
        ElapsedUs(t_xfer0, std::chrono::steady_clock::now()), std::memory_order_relaxed);

    // ---- Validate: parent fully drained + every move landed. ----
    const bool drained =
        parent.Entities().empty() && parent.Players().empty() && parent.NetBySession().empty();
    bool landed = drained;
    if (landed) {
        for (const auto& move : moves) {
            if (!zones_.GetZone(move.target_index).FindEntity(move.net_id).is_valid()) {
                landed = false;
                break;
            }
        }
    }
    if (!landed) {
        return abort_split("validate-failed", moves);
    }

    // ---- Commit: tree flips + directory publish + counts. ----
    const auto t_commit0 = std::chrono::steady_clock::now();
    if (!zones_.CommitSplit(zone_id, children)) {
        return abort_split("commit-refused", moves);
    }
    for (const ZoneId child_id : children) {
        directory_.SetAssignment(child_id, child_locs[child_id]);
    }
    directory_.RetireZones({zone_id});
    parent.RefreshResidentCounts();
    for (const ZoneId child_id : children) {
        zones_.GetZone(zones_.FindIndexById(child_id)).RefreshResidentCounts();
    }
    partition_metrics_.split_commits.fetch_add(1, std::memory_order_relaxed);
    partition_metrics_.split_commit_us.fetch_add(
        ElapsedUs(t_commit0, std::chrono::steady_clock::now()), std::memory_order_relaxed);
    LOG_INFO("partition: split {} zone={} children={} moved={}",
             forced ? "FORCED-COMMIT" : "COMMIT",
             zone_id,
             children.size(),
             moves.size());
    return true;
}

bool WorldRuntime::RunMergeTransaction(ZoneId parent_node_id, bool forced)
{
    const auto t_plan0 = std::chrono::steady_clock::now();
    ZoneManager::MergePlan plan;
    if (!zones_.PlanMerge(parent_node_id, plan)) {
        return false; // routine skip, not an abort
    }
    for (const ZoneId child_id : plan.child_ids) {
        if (ZoneHasPendingMigration(child_id)) {
            LOG_INFO("partition: merge deferred parent={} (racing migration, retry next cycle)",
                     parent_node_id);
            return false;
        }
    }
    partition_metrics_.merge_plan_us.fetch_add(ElapsedUs(t_plan0, std::chrono::steady_clock::now()),
                                               std::memory_order_relaxed);
    partition_metrics_.merge_attempts.fetch_add(1, std::memory_order_relaxed);

    ZoneId merged_id = 0;
    if (!zones_.CreateStagedMergeTarget(plan, merged_id)) {
        partition_metrics_.merge_aborts.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("partition: merge ABORTED parent={} (staging failed, children restored)",
                 parent_node_id);
        return false;
    }
    const std::size_t merged_index = zones_.FindIndexById(merged_id);
    const auto loc = directory_.ResolveZone(parent_node_id);
    ZoneLocation merged_loc = loc.value_or(LocalZoneLocation(identity_, merged_id));
    merged_loc.zone = merged_id;

    auto abort_merge = [&](const char* reason, std::vector<std::uint32_t>& moved_from,
                           const std::unordered_map<std::uint32_t, ZoneId>& home) -> bool {
        const auto t_rb0 = std::chrono::steady_clock::now();
        std::uint64_t rb_fail = 0;
        for (auto it = moved_from.rbegin(); it != moved_from.rend(); ++it) {
            const auto home_it = home.find(*it);
            if (home_it == home.end()) {
                ++rb_fail;
                continue;
            }
            ZoneLocation back = loc.value_or(LocalZoneLocation(identity_, home_it->second));
            back.zone = home_it->second;
            const std::size_t home_index = zones_.FindIndexById(home_it->second);
            if (!TransferResident(merged_index, home_index, back, *it)) {
                ++rb_fail;
                LOG_ERROR("partition: merge rollback failed net_id={} (stays staged; validator "
                          "will flag loudly)",
                          *it);
            }
        }
        zones_.AbortMerge(plan, merged_id);
        partition_metrics_.merge_aborts.fetch_add(1, std::memory_order_relaxed);
        partition_metrics_.merge_rollback_failures.fetch_add(rb_fail, std::memory_order_relaxed);
        partition_metrics_.merge_rollback_us.fetch_add(
            ElapsedUs(t_rb0, std::chrono::steady_clock::now()), std::memory_order_relaxed);
        LOG_WARN("partition: merge ABORTED parent={} reason={} moved={} rb_fail={} (children "
                 "restored)",
                 parent_node_id,
                 reason,
                 moved_from.size(),
                 rb_fail);
        return false;
    };

    // ---- Transfer: every child resident into the staged target. ----
    const auto t_xfer0 = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> moved;
    std::unordered_map<std::uint32_t, ZoneId> home; // net -> child ZoneId for rollback
    for (const ZoneId child_id : plan.child_ids) {
        const std::size_t child_index = zones_.FindIndexById(child_id);
        if (child_index >= zones_.ZoneCount()) {
            return abort_merge("child-vanished", moved, home);
        }
        Zone& child = zones_.GetZone(child_index);
        std::vector<std::uint32_t> residents;
        for (const auto& [net_id, entity] : child.Entities()) {
            if (!entity.is_valid() || !entity.has<Position>()) {
                return abort_merge("stale-resident", moved, home);
            }
            residents.push_back(net_id);
        }
        for (const std::uint32_t net_id : residents) {
            if (!TransferResident(child_index, merged_index, merged_loc, net_id)) {
                partition_metrics_.merge_transfer_failures.fetch_add(1, std::memory_order_relaxed);
                return abort_merge("transfer-failed", moved, home);
            }
            moved.push_back(net_id);
            home.emplace(net_id, child_id);
        }
        child.RefreshResidentCounts();
    }
    partition_metrics_.merge_transfer_us.fetch_add(
        ElapsedUs(t_xfer0, std::chrono::steady_clock::now()), std::memory_order_relaxed);

    // ---- Validate: children drained + merged holds everything. ----
    bool ok = true;
    for (const ZoneId child_id : plan.child_ids) {
        const Zone& child = zones_.GetZone(zones_.FindIndexById(child_id));
        if (!child.Entities().empty() || !child.Players().empty() ||
            !child.NetBySession().empty()) {
            ok = false;
            break;
        }
    }
    if (ok) {
        Zone& merged = zones_.GetZone(merged_index);
        for (const std::uint32_t net_id : moved) {
            if (!merged.FindEntity(net_id).is_valid()) {
                ok = false;
                break;
            }
        }
        merged.RefreshResidentCounts();
    }
    if (!ok) {
        return abort_merge("validate-failed", moved, home);
    }

    // ---- Commit: tree collapse + children retire + directory publish. ----
    const auto t_commit0 = std::chrono::steady_clock::now();
    if (!zones_.CommitMerge(plan, merged_id)) {
        return abort_merge("commit-refused", moved, home);
    }
    directory_.RetireZones(plan.child_ids);
    ZoneLocation merged_dir_loc = loc.value_or(LocalZoneLocation(identity_, merged_id));
    merged_dir_loc.zone = merged_id;
    directory_.SetAssignment(merged_id, merged_dir_loc);
    partition_metrics_.merge_commits.fetch_add(1, std::memory_order_relaxed);
    partition_metrics_.merge_commit_us.fetch_add(
        ElapsedUs(t_commit0, std::chrono::steady_clock::now()), std::memory_order_relaxed);
    LOG_INFO("partition: merge {} parent={} merged={} children={} moved={}",
             forced ? "FORCED-COMMIT" : "COMMIT",
             parent_node_id,
             merged_id,
             plan.child_ids.size(),
             moved.size());
    return true;
}

bool WorldRuntime::TransferResident(std::size_t source_zone_index,
                                    std::size_t target_zone_index,
                                    ZoneLocation target_location,
                                    std::uint32_t net_id)
{
    if (source_zone_index >= zones_.ZoneCount() || target_zone_index >= zones_.ZoneCount() ||
        source_zone_index == target_zone_index) {
        return false;
    }
    // Ordered acquisition (by index) keeps the two-zone scope deadlock-free.
    if (source_zone_index < target_zone_index) {
        ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "partition transfer source");
        ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "partition transfer target");
        return TransferResidentLocked(zones_.GetZone(source_zone_index),
                                      zones_.GetZone(target_zone_index), target_zone_index,
                                      target_location, net_id);
    }
    ZoneWriteGuard target_guard(zones_.GetZone(target_zone_index), "partition transfer target");
    ZoneWriteGuard source_guard(zones_.GetZone(source_zone_index), "partition transfer source");
    return TransferResidentLocked(zones_.GetZone(source_zone_index),
                                  zones_.GetZone(target_zone_index), target_zone_index,
                                  target_location, net_id);
}

bool WorldRuntime::TransferResidentLocked(Zone& source_zone,
                                          Zone& target_zone,
                                          std::size_t target_zone_index,
                                          ZoneLocation target_location,
                                          std::uint32_t net_id)
{
    const auto entity = source_zone.FindEntity(net_id);
    if (!entity.is_valid()) {
        return false;
    }
    const bool is_player = entity.has<PlayerTag>();
    gs::common::SessionId session_id = 0;
    if (is_player) {
        const auto* binding = source_zone.FindPlayer(net_id);
        if (binding == nullptr) {
            return false;
        }
        session_id = binding->session ? binding->session->Id() : 0;
    }

    // Failure-injection grace window: let the first N transfers succeed so
    // mid-batch aborts are reproducible. Single supervisor consumer, so a
    // plain load/store pair is race-free in practice.
    bool failure_armed = true;
    const int grace = test_fail_after_count_.load(std::memory_order_relaxed);
    if (grace > 0) {
        test_fail_after_count_.store(grace - 1, std::memory_order_relaxed);
        failure_armed = false;
    }

    // Injected snapshot-stage failure: before ANY mutation, source untouched.
    if (failure_armed && ConsumeTestFailure(test_fail_snapshot_count_)) {
        LOG_WARN("partition: injected snapshot failure net_id={} (source untouched)", net_id);
        return false;
    }

    // Same authority-transfer primitive as MigrationCoordinator, but
    // apply-first: the source is released only after the target accepted.
    EntityTransfer transfer;
    try {
        transfer = BuildTransfer(entity, is_player, NamespaceFor(identity_));
    } catch (const std::exception& error) {
        LOG_ERROR("partition: net_id={} snapshot failed: {}", net_id, error.what());
        return false;
    } catch (...) {
        LOG_ERROR("partition: net_id={} snapshot failed (unknown)", net_id);
        return false;
    }

    transfer.position.z = terrain_.SampleGroundHeight(transfer.position.x, transfer.position.y);
    DestinationRollback rollback;
    rollback.target = &target_zone;
    rollback.net_id = net_id;
    rollback.position = transfer.position;
    try {
        GhostSystem::RemoveByNetId(target_zone, net_id);
        rollback.entity = ApplyTransfer(target_zone.World(), transfer);
        rollback.applied = true;
        target_zone.IndexEntity(net_id, rollback.entity);
        rollback.indexed = true;
        target_zone.Grid().Insert(net_id, transfer.position);
        rollback.gridded = true;
    } catch (const std::exception& error) {
        LOG_ERROR("partition: net_id={} apply failed, source untouched: {}", net_id, error.what());
        return false; // guard destroys the partial destination
    } catch (...) {
        LOG_ERROR("partition: net_id={} apply failed (unknown), source untouched", net_id);
        return false; // guard destroys the partial destination
    }

    // Injected apply-stage failure: destination fully built, source still
    // authoritative. The guard must remove every destination trace.
    if (failure_armed && ConsumeTestFailure(test_fail_apply_count_)) {
        LOG_WARN("partition: injected apply failure net_id={} (rollback engages)", net_id);
        return false;
    }

    // Move session/RNG state. If anything here throws, extracted state is
    // restored to the source and the guard rolls the destination back, so
    // neither side is left partial.
    Zone::PlayerBinding moved_binding;
    std::optional<std::mt19937> moved_rng;
    try {
        if (is_player) {
            moved_binding = source_zone.ExtractPlayerBinding(net_id);
            target_zone.InsertPlayerBinding(net_id, std::move(moved_binding));
            rollback.bound = true;
        } else {
            moved_rng = source_zone.ExtractMobRng(net_id);
            if (moved_rng) {
                target_zone.InsertMobRng(net_id, std::move(*moved_rng));
                rollback.rng_moved = true;
            }
        }
    } catch (const std::exception& error) {
        LOG_ERROR("partition: net_id={} state move failed: {}", net_id, error.what());
        try {
            if (is_player) {
                source_zone.InsertPlayerBinding(net_id, std::move(moved_binding));
            } else if (moved_rng) {
                source_zone.InsertMobRng(net_id, std::move(*moved_rng));
            }
        } catch (...) {
            // Source restore is best-effort; the entity itself was never
            // released, so authority never forked.
        }
        return false; // guard rolls back the destination
    } catch (...) {
        LOG_ERROR("partition: net_id={} state move failed (unknown)", net_id);
        return false;
    }

    // ---- COMMIT POINT (§10): release the source. From here the destination
    // is the single authority. Unobservable before this line: both write
    // guards held, no tick in flight, staged zones unscheduled/unrouted. ----
    source_zone.Grid().Remove(net_id, transfer.position);
    entity.destruct();
    source_zone.UnindexEntity(net_id);
    source_zone.EraseMobRng(net_id);
    rollback.commit();
    if (!is_player) {
        // LOD state rode along in the transfer payload.
        target_zone.NoteLodInsert(transfer.sim_lod.tier);
    }

    // Routing follows authority (post-commit; allocation failure here is
    // fatal-class and cannot fork authority).
    if (is_player && session_id != 0) {
        OwnerInfo owner;
        owner.entity = transfer.entity_id;
        owner.location = target_location;
        owner.zone_index = target_zone_index;
        owner.net_id = net_id;
        owners_by_session_[session_id] = owner;
    }
    return true;
}

void WorldRuntime::ConfigurePartition(const PartitionConfig& config)
{
    const auto validated = ValidatePartitionConfig(config);
    for (const auto& warning : validated.warnings) {
        LOG_WARN("{}", warning);
    }
    const PartitionConfig& e = validated.effective;

    ZoneLoadMonitor::Config monitor;
    monitor.split_load_threshold = e.split_load_threshold;
    monitor.merge_load_threshold = e.merge_load_threshold;
    monitor.sustained_window = std::chrono::seconds(e.sustained_window_seconds);
    monitor.split_cooldown = std::chrono::seconds(e.split_cooldown_seconds);
    monitor.merge_cooldown = std::chrono::seconds(e.merge_cooldown_seconds);
    monitor.tick_budget_ms = e.tick_budget_ms;
    monitor.resident_budget = e.resident_budget;
    load_monitor_ = ZoneLoadMonitor(monitor); // resets sustained timers; call pre-Start or idle

    scheduler_.config.split_load_threshold = e.split_load_threshold;
    scheduler_.config.merge_load_threshold = e.merge_load_threshold;
    scheduler_.config.sustained_window = std::chrono::seconds(e.sustained_window_seconds);
    scheduler_.config.split_cooldown = std::chrono::seconds(e.split_cooldown_seconds);
    scheduler_.config.merge_cooldown = std::chrono::seconds(e.merge_cooldown_seconds);
    scheduler_.config.max_depth = static_cast<std::uint8_t>(e.max_partition_depth);

    zones_.ApplyRegionLimits(e.max_partition_depth, e.min_zone_size_m);
    effective_partition_config_ = e;

    LOG_INFO("partition config effective: split>{:.2f} merge<{:.2f} sustained={}s cooldowns={}s/"
             "{}s tick_budget={:.1f}ms residents={:.0f} depth<={} minsize={:.0f}m",
             e.split_load_threshold,
             e.merge_load_threshold,
             e.sustained_window_seconds,
             e.split_cooldown_seconds,
             e.merge_cooldown_seconds,
             e.tick_budget_ms,
             e.resident_budget,
             e.max_partition_depth,
             e.min_zone_size_m);
}

void WorldRuntime::ConfigureSimulationLod(const LodConfig& config)
{
    const auto validated = ValidateLodConfig(config);
    for (const auto& warning : validated.warnings) {
        LOG_WARN("{}", warning);
    }
    effective_lod_config_ = validated.effective;
    scheduler_.SetLodEnabled(validated.effective.enabled);
    const LodConfig& e = validated.effective;
    LOG_INFO("simulation lod effective: enabled={} bubbles=[{:.0f}/{:.0f}/{:.0f}]m hz=[20/{:.1f}/{:.1f}] "
             "demote=[{:.0f}/{:.0f}/{:.0f}]s",
             e.enabled,
             e.full_radius_m,
             e.reduced_radius_m,
             e.low_radius_m,
             e.reduced_hz,
             e.low_hz,
             e.demote_full_sec,
             e.demote_reduced_sec,
             e.demote_low_sec);
}

void WorldRuntime::PostForceSplit(ZoneId zone_id)
{
    Enqueue([this, zone_id] {
        ExecuteForcedSplit(zone_id);
    });
}

void WorldRuntime::PostForceMerge(ZoneId parent_node_id)
{
    Enqueue([this, parent_node_id] {
        ExecuteForcedMerge(parent_node_id);
    });
}

void WorldRuntime::InjectTransferFailuresForTest(int snapshot_failures, int apply_failures,
                                                  int succeed_first)
{
    test_fail_snapshot_count_.store(snapshot_failures < 0 ? 0 : snapshot_failures,
                                    std::memory_order_relaxed);
    test_fail_apply_count_.store(apply_failures < 0 ? 0 : apply_failures,
                                 std::memory_order_relaxed);
    test_fail_after_count_.store(succeed_first < 0 ? 0 : succeed_first,
                                 std::memory_order_relaxed);
}

void WorldRuntime::DrainGlobalCommands()
{
    for (;;) {
        std::function<void()> command;
        {
            std::lock_guard lock(mutex_);
            if (commands_.empty()) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop();
        }
        // A failing global command must not kill the supervisor loop.
        try {
            command();
        } catch (const std::exception& error) {
            LOG_ERROR("Game sim global command failed: {}", error.what());
        } catch (...) {
            LOG_ERROR("Game sim global command failed with unknown exception");
        }
    }
}

} // namespace gs::game
