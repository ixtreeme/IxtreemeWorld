#include "WorldRuntime.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include "common/Logging.h"

#include "replication/NetworkSend.h"
#include "systems/CombatSystem.h"
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
            }
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
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} sleeping_zones={} active_sessions={} active_mobs={} wandering_mobs={} idle_mobs={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} attacks_per_sec={} deaths_total={} respawns_pending={} respawns_total={} migrations={} mig_pending={} mig_quarantined={} mig_detail=[c={} stale={} dup={} retry={} fail={}] routes=[local={} remu={} unav={} drain={} miss={}] workers={} worker_busy_pct={:.1f}                      avg_zone_tick_ms={:.3f} aoi_queries={} dirty_xf={} tiers=[{}/{}/{}] stage_us=[gameplay={} ghost={} repl={}] avg_supervisor_ms={:.3f}",
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
                     avg_supervisor_ms);
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
