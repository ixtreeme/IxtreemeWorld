#include "WorldRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
    // Size the activity grid to the real world extent (test map and 100km
    // world alike); positions clamp into it by construction.
    activity_field_.Reconfigure(
        SpatialActivityField::Config{kActivityCellSizeMeters,
                                    WorldBounds::FromExtent(terrain_.WorldExtentMeters())});
    // Load field: same world extent, independent cell size (config). The
    // mapping is bound to every zone so their local load bins line up with
    // the generation grid before any tick can run.
    LoadFieldConfig load_field_config;
    load_field_config.bounds = WorldBounds::FromExtent(terrain_.WorldExtentMeters());
    load_field_.Reconfigure(load_field_config);
    {
        LoadFieldMapping mapping = LoadFieldMapping::FromConfig(load_field_config);
        mapping.valid = load_field_config.enabled; // disabled = zero-cost bins
        zones_.ApplyLoadFieldMapping(mapping);
    }
    effective_load_field_config_ = load_field_config;
    last_load_field_build_ = std::chrono::steady_clock::now();
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
                        activity_field_.Snapshot(),
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
        // World-space activity rebuild (~1Hz, no tick gating: per-zone
        // buffer copies only). Feeds this pass's LOD evaluations (via tick
        // contexts) and sleep/wake decisions below.
        const auto now_activity = std::chrono::steady_clock::now();
        if (now_activity - last_activity_build_ >= std::chrono::seconds(1)) {
            last_activity_build_ = now_activity;
            const auto& lod = effective_lod_config_;
            activity_field_.Rebuild(zones_,
                                    ActivityRadii{lod.full_radius_m, lod.reduced_radius_m,
                                                  lod.low_radius_m},
                                    lod.enabled);
        }
        const auto activity_snapshot = activity_field_.Snapshot();
        // Continuous load field aggregation (configurable cadence, default
        // ~1Hz). Drains the load deltas zones published at the end of their
        // last ticks; the dt is the real elapsed time so the asymmetric EMAs
        // stay cadence independent.
        const auto now_load_field = std::chrono::steady_clock::now();
        const double load_field_interval =
            1.0 / static_cast<double>(effective_load_field_config_.aggregation_hz);
        if (std::chrono::duration<double>(now_load_field - last_load_field_build_).count() >=
            load_field_interval) {
            const double dt =
                std::chrono::duration<double>(now_load_field - last_load_field_build_).count();
            last_load_field_build_ = now_load_field;
            load_field_.Rebuild(zones_, dt);
        }
        // Wake radius derives from the LOD reduced radius (§20): any player
        // inside it may grant Full/Reduced relevance, so the zone must tick.
        scheduler_.ScheduleOnce(zones_,
                                workers_,
                                std::chrono::steady_clock::now(),
                                activity_snapshot,
                                effective_lod_config_.reduced_radius_m);
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
                const auto activity_for_validation = activity_field_.Snapshot();
                const bool ok =
                    ValidateWorldConsistency(zones_, owners_by_session_, migration_queue_, directory_,
                                             activity_for_validation.get(), error);
                std::lock_guard lock(validation_mutex_);
                validation_result_ = ok ? std::string("OK") : "FAIL: " + error;
                validation_ready_ = true;
            }
        }
        // Continuous load field self-consistency audit: same quiescent
        // window, explicit request only (the raw work events are already
        // consumed, so this audits the generation's internal invariants).
        if (load_field_validation_requested_.load(std::memory_order_relaxed)) {
            if (!zones_.AnyTickInProgress()) {
                load_field_validation_requested_.store(false, std::memory_order_relaxed);
                std::string error;
                const auto load_field_for_validation = load_field_.Snapshot();
                const bool ok = load_field_for_validation != nullptr &&
                                ValidateLoadFieldGrid(*load_field_for_validation, error);
                std::lock_guard lock(load_field_validation_mutex_);
                load_field_validation_result_ = ok ? std::string("OK") : "FAIL: " + error;
                load_field_validation_ready_ = true;
            }
        }
        // Strict field-vs-brute-force audit (§31): same quiescent window,
        // explicit request only (static scenarios; roaming load would race
        // the 1Hz snapshot). Samples + outcome are stashed for the bench.
        if (activity_validation_requested_.load(std::memory_order_relaxed)) {
            if (!zones_.AnyTickInProgress()) {
                activity_validation_requested_.store(false, std::memory_order_relaxed);
                const std::size_t max_samples =
                    activity_validation_max_samples_.load(std::memory_order_relaxed);
                std::vector<ActivitySampleResult> samples;
                std::string error;
                const bool ok = ValidateActivityFieldDetailed(
                    zones_, *activity_field_.Snapshot(), samples, error, max_samples);
                std::lock_guard lock(activity_validation_mutex_);
                activity_samples_ = std::move(samples);
                activity_validation_error_ = std::move(error);
                activity_validation_ok_ = ok;
                activity_samples_ready_ = true;
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
            std::uint64_t total_x_full = 0;
            std::uint64_t total_x_reduced = 0;
            std::uint64_t total_x_low = 0;
            std::uint64_t total_sleep_block = 0;
            std::uint64_t total_wake_ext = 0;
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
                total_x_full += zone.Diagnostics().cross_zone_full_since_diag.exchange(0);
                total_x_reduced += zone.Diagnostics().cross_zone_reduced_since_diag.exchange(0);
                total_x_low += zone.Diagnostics().cross_zone_low_since_diag.exchange(0);
                total_sleep_block += zone.Diagnostics().sleep_blocked_external_since_diag.exchange(0);
                total_wake_ext += zone.Diagnostics().wake_external_since_diag.exchange(0);
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
             const auto activity_metrics = activity_field_.Metrics();
            LOG_INFO("Game sim diag: world_tick={} zones={} active_zones={} sleeping_zones={} active_sessions={} active_mobs={} wandering_mobs={} idle_mobs={} ghosts={} zone_ticks={} empty_zone_skips={} transform_records_sent={} attacks_per_sec={} deaths_total={} respawns_pending={} respawns_total={} migrations={} mig_pending={} mig_quarantined={} mig_detail=[c={} stale={} dup={} retry={} fail={}] routes=[local={} remu={} unav={} drain={} miss={}] workers={} worker_busy_pct={:.1f}                      avg_zone_tick_ms={:.3f} aoi_queries={} dirty_xf={} tiers=[{}/{}/{}] stage_us=[gameplay={} ghost={} repl={}] avg_supervisor_ms={:.3f} partition=[s_att={} s_ok={} s_ab={} m_att={} m_ok={} m_ab={} rej={}] lod=[{}/{}/{}/{} ai={} mv={} prom={} dem={} wake={} eval_us={}] xzone=[f={} r={} l={}] sleep=[blocked={} wext={}] activity=[srcs={} cells={} rb_us={}]",
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
                     total_lod_eval_us,
                     total_x_full,
                     total_x_reduced,
                     total_x_low,
                     total_sleep_block,
                     total_wake_ext,
                     activity_metrics.sources,
                     activity_metrics.cells_nonempty,
                     activity_metrics.rebuild_us_total);
            // Continuous load field: one concise line per diag window. The
            // channel breakdown stays visible (raw units per window) plus the
            // peak normalized/composite view; per-cell data is queried
            // through LoadFieldSnapshot().
            const auto lf = load_field_.Metrics();
            LOG_INFO("Load field diag: epoch={} cells={} active={} l1={}/{} rb_us={} entries={} "
                     "totals=[sim={} bytes={} records={} dirty={} aoi_q={} aoi_c={} combat={} mig={}] "
                     "peak=[norm={:.3f} comp={:.3f}]",
                     lf.epoch,
                     lf.cells_total,
                     lf.cells_active,
                     lf.l1_cells_active,
                     lf.l1_cells_total,
                     lf.last_rebuild_us,
                     lf.drained_entries,
                     lf.last_totals.sim_work,
                     lf.last_totals.repl_bytes,
                     lf.last_totals.repl_records,
                     lf.last_totals.repl_dirty,
                     lf.last_totals.aoi_queries,
                     lf.last_totals.aoi_candidates,
                     lf.last_totals.combat_events,
                     lf.last_totals.migration_events,
                     lf.peak_normalized,
                     lf.peak_composite);
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
    const auto activity = activity_field_.Snapshot();
    return ValidateWorldConsistency(zones_, owners_by_session_, migration_queue_, directory_,
                                    activity.get(), out_error);
}

void WorldRuntime::RequestActivityValidation(std::size_t max_samples)
{
    activity_validation_max_samples_.store(max_samples, std::memory_order_relaxed);
    activity_validation_requested_.store(true, std::memory_order_relaxed);
}

bool WorldRuntime::TryTakeActivitySamples(std::vector<ActivitySampleResult>& out_samples,
                                          std::string& out_error)
{
    std::lock_guard lock(activity_validation_mutex_);
    if (!activity_samples_ready_) {
        return false;
    }
    out_samples = activity_samples_;
    out_error = activity_validation_error_;
    activity_samples_.clear();
    activity_validation_error_.clear();
    activity_samples_ready_ = false;
    return activity_validation_ok_;
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

    // OBSERVE: legacy p95/p99 tick + resident pressure, field overload score,
    // sustained timers, candidate lists.
    const auto field = load_field_.Snapshot();
    const auto activity = activity_field_.Snapshot();
    load_monitor_.Update(zones_, scheduler_, now, field);

    const PartitionScoringConfig& scoring = scorer_.GetConfig();

    // SCORE + DECIDE: at most one topology mutation per control cycle. The
    // scorer only recommends; the transactional executors re-validate every
    // gate. Explicit deterministic priority (§25): a split is an active
    // compute pressure and always goes first; a merge is an optimization and
    // only runs when no split executed.
    bool mutated = false;
    for (const ZoneId zone_id : load_monitor_.SplitCandidates()) {
        const SplitRecommendation recommendation =
            ScorePartitionWith(zone_id, field, activity, now);
        const float p99_ms = [&] {
            for (const auto& snap : load_monitor_.RecentSnapshots()) {
                if (snap.zone_id == zone_id) {
                    return snap.p99_tick_us / 1000.0f;
                }
            }
            return 0.0f;
        }();
        // Emergency bypass accounting: the gate already passed, so
        // re-evaluating it only tells us whether the merge-to-split cooldown
        // was overridden by the measured signal.
        {
            const ZonePartition* leaf = FindZoneNode(zone_id);
            bool emergency = false;
            if (leaf != nullptr) {
                (void)scheduler_.EvaluateSplitGate(leaf, now, &emergency);
            }
            if (emergency) {
                partition_metrics_.split_emergency_bypasses.fetch_add(1,
                                                                      std::memory_order_relaxed);
            }
        }
        if (recommendation.valid &&
            recommendation.best.final_score >= scoring.min_expected_improvement) {
            const bool committed =
                RunSplitTransaction(zone_id, false, &recommendation.best.center);
            PartitionDecisionRecord record;
            record.timestamp = now;
            record.zone_id = zone_id;
            record.executed = committed;
            record.scored = true;
            record.noop_reason = committed ? PartitionNoopReason::None
                                           : PartitionNoopReason::TransactionRejected;
            record.p99_tick_ms = p99_ms;
            record.field_epoch = recommendation.field_epoch;
            record.expected_improvement = recommendation.expected_improvement;
            record.candidate = recommendation.best;
            for (const auto& snap : load_monitor_.RecentSnapshots()) {
                if (snap.zone_id == zone_id) {
                    record.load_score = snap.load_score;
                    record.field_load_score = snap.field_load_score;
                    break;
                }
            }
            RecordPartitionDecision(std::move(record), committed, false);
            if (committed) {
                mutated = true;
                NoteOscillationIfAny(FindZoneNode(zone_id), true, now);
            }
        } else {
            PartitionDecisionRecord record;
            record.timestamp = now;
            record.zone_id = zone_id;
            record.scored = recommendation.valid;
            record.noop_reason = recommendation.valid ? PartitionNoopReason::BelowMinImprovement
                                                      : PartitionNoopReason::NoValidCut;
            record.p99_tick_ms = p99_ms;
            record.field_epoch = recommendation.field_epoch;
            record.expected_improvement = recommendation.expected_improvement;
            record.candidate = recommendation.best;
            for (const auto& snap : load_monitor_.RecentSnapshots()) {
                if (snap.zone_id == zone_id) {
                    record.load_score = snap.load_score;
                    record.field_load_score = snap.field_load_score;
                    break;
                }
            }
            RecordPartitionDecision(std::move(record), false, true);
        }
        break;
    }

    // Why-not diagnostics (§31): sustained-overloaded leaves blocked by a
    // hard gate (cooldown/depth/size/commands) get one structured record.
    // Rate-limited so a stuck zone cannot spam the log.
    if (!mutated) {
        for (const ZoneId zone_id : load_monitor_.OverloadedLeaves()) {
            bool already_candidate = false;
            for (const ZoneId candidate : load_monitor_.SplitCandidates()) {
                if (candidate == zone_id) {
                    already_candidate = true;
                    break;
                }
            }
            if (already_candidate) {
                continue; // scored path above already recorded it
            }
            const std::size_t zone_index = zones_.FindIndexById(zone_id);
            if (zone_index >= zones_.ZoneCount()) {
                continue;
            }
            // The scheduler gate explains sustained/cooldown blocks; the plan
            // dry-run explains geometry (depth/min-size/commands). The
            // structured detail string names the actual blocker.
            const ZonePartition* leaf = nullptr;
            for (const auto& root : zones_.PartitionRoots()) {
                if (const auto* found = FindPartitionNode(root.get(), zone_id)) {
                    leaf = found;
                    break;
                }
            }
            const auto gate = scheduler_.EvaluateSplitGate(leaf, now);
            if (gate == ZoneScheduler::SplitGate::MergeToSplitCooldown) {
                partition_metrics_.split_suppressed_merge_cooldown.fetch_add(
                    1, std::memory_order_relaxed);
            }
            SplitRejectReason plan_reason = SplitRejectReason::None;
            ZoneManager::SplitPlan plan;
            (void)zones_.PlanSplit(zone_id, plan, &plan_reason);
            PartitionDecisionRecord record;
            record.timestamp = now;
            record.zone_id = zone_id;
            record.scored = false;
            // The structured reason names the class of blocker; the detail
            // literal names the exact gate.
            switch (gate) {
            case ZoneScheduler::SplitGate::NotSustained:
                record.noop_reason = PartitionNoopReason::SplitNotSustained;
                break;
            case ZoneScheduler::SplitGate::Cooldown:
            case ZoneScheduler::SplitGate::MergeToSplitCooldown:
                record.noop_reason = PartitionNoopReason::SplitCooldown;
                break;
            case ZoneScheduler::SplitGate::MaxDepth:
                record.noop_reason = PartitionNoopReason::SplitMaxDepth;
                break;
            case ZoneScheduler::SplitGate::NotLeaf:
            case ZoneScheduler::SplitGate::BelowThreshold:
            case ZoneScheduler::SplitGate::Pass:
            default:
                record.noop_reason = plan_reason == SplitRejectReason::TooSmall
                                         ? PartitionNoopReason::SplitMinSize
                                         : PartitionNoopReason::SplitNotEligible;
                break;
            }
            record.detail = gate != ZoneScheduler::SplitGate::Pass
                                ? ZoneScheduler::SplitGateName(gate)
                                : SplitRejectReasonName(plan_reason);
            for (const auto& snap : load_monitor_.RecentSnapshots()) {
                if (snap.zone_id == zone_id) {
                    record.p99_tick_ms = snap.p99_tick_us / 1000.0f;
                    record.load_score = snap.load_score;
                    record.field_load_score = snap.field_load_score;
                    break;
                }
            }
            record.field_epoch = field != nullptr ? field->epoch : 0;
            RecordPartitionDecision(std::move(record), false, true);
            break;
        }
    }

    // ---- MERGE: only when no split executed (explicit priority above) ----
    if (!mutated) {
        // Gate-state counters for every observed group (deterministic tree
        // order). These make the stability controller tunable: the report
        // shows WHY the tree is not simplifying.
        for (const auto& group : load_monitor_.MergeGroups()) {
            switch (group.gate) {
            case ZoneScheduler::MergeGate::NotSustained:
                if (group.sustained_low_seconds > 0.0f) {
                    partition_metrics_.merge_suppressed_not_sustained.fetch_add(
                        1, std::memory_order_relaxed);
                }
                break;
            case ZoneScheduler::MergeGate::SplitToMergeCooldown:
                partition_metrics_.merge_suppressed_recent_split.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ZoneScheduler::MergeGate::MergeCooldown:
                partition_metrics_.merge_suppressed_recent_merge.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ZoneScheduler::MergeGate::Root:
            case ZoneScheduler::MergeGate::NoParent:
            case ZoneScheduler::MergeGate::NotFourChildren:
            case ZoneScheduler::MergeGate::ChildNotLeaf:
                partition_metrics_.merge_suppressed_not_eligible.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ZoneScheduler::MergeGate::Pass:
            default:
                break;
            }
        }

        // SCORE every gate-passing group; the best recommendation wins
        // deterministically (higher score, then earlier tree order).
        MergeRecommendation best;
        bool have_best = false;
        for (const ZoneId parent_id : load_monitor_.MergeCandidates()) {
            const MergeRecommendation recommendation =
                ScoreMergeWith(parent_id, field, activity, now);
            partition_metrics_.merge_candidates_evaluated.fetch_add(1,
                                                                    std::memory_order_relaxed);
            if (!recommendation.valid) {
                partition_metrics_.merge_suppressed_not_eligible.fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            if (!recommendation.best.safety_ok) {
                partition_metrics_.merge_suppressed_post_merge_unsafe.fetch_add(
                    1, std::memory_order_relaxed);
                RecordMergeDecision(recommendation, PartitionNoopReason::MergeUnsafePostMerge,
                                    "post-merge-unsafe", false, now);
                continue;
            }
            if (recommendation.best.final_score < scoring.min_merge_improvement) {
                partition_metrics_.merge_suppressed_min_improvement.fetch_add(
                    1, std::memory_order_relaxed);
                RecordMergeDecision(recommendation, PartitionNoopReason::MergeBelowMinImprovement,
                                    "below-min-improvement", false, now);
                continue;
            }
            if (!have_best || recommendation.best.final_score > best.best.final_score) {
                best = recommendation;
                have_best = true;
            }
        }
        if (have_best) {
            const bool committed = RunMergeTransaction(best.parent_id, false);
            if (!committed) {
                partition_metrics_.merge_suppressed_transaction.fetch_add(
                    1, std::memory_order_relaxed);
            }
            RecordMergeDecision(best,
                                committed ? PartitionNoopReason::None
                                          : PartitionNoopReason::MergeTransactionRejected,
                                committed ? "executed" : "transaction-rejected", committed, now);
            if (committed) {
                mutated = true;
                NoteOscillationIfAny(FindZoneNode(best.parent_id), false, now);
            }
        } else {
            // Why-not: the first group that is in the merge funnel (low load,
            // timer running, gate not yet passed) gets one rate-limited
            // structured record so the controller is not opaque.
            for (const auto& group : load_monitor_.MergeGroups()) {
                if (group.candidate || group.sustained_low_seconds <= 0.0f) {
                    continue;
                }
                RecordMergeGateNoop(group, now);
                break;
            }
        }
    }
}

SplitRecommendation WorldRuntime::ScorePartitionWith(
    ZoneId zone_id,
    const std::shared_ptr<const LoadGrid>& field,
    const std::shared_ptr<const ActivityGrid>& activity,
    std::chrono::steady_clock::time_point now) const
{
    const std::size_t zone_index = zones_.FindIndexById(zone_id);
    if (zone_index >= zones_.ZoneCount()) {
        return SplitRecommendation{};
    }
    const Zone& zone = zones_.GetZone(zone_index);
    PartitionScoreInput input;
    input.zone_id = zone_id;
    input.bounds = zone.Bounds();
    input.players = zone.Diagnostics().player_count.load(std::memory_order_relaxed);
    input.mobs = zone.Diagnostics().mob_count.load(std::memory_order_relaxed);
    for (const auto& root : zones_.PartitionRoots()) {
        if (const auto* leaf = FindPartitionNode(root.get(), zone_id)) {
            input.depth = leaf->depth;
            input.last_mutation = leaf->last_split_time;
            if (leaf->parent != nullptr && leaf->parent->last_merge_time > input.last_mutation) {
                input.last_mutation = leaf->parent->last_merge_time;
            }
            break;
        }
    }
    SplitRecommendation recommendation = scorer_.ScoreSplit(input, field.get(), activity.get(), now);
    recommendation.activity_epoch = activity != nullptr ? activity->epoch : 0;
    return recommendation;
}

SplitRecommendation WorldRuntime::ScorePartition(ZoneId zone_id) const
{
    return ScorePartitionWith(zone_id, load_field_.Snapshot(), activity_field_.Snapshot(),
                              std::chrono::steady_clock::now());
}

const ZonePartition* WorldRuntime::FindZoneNode(ZoneId zone_id) const
{
    for (const auto& root : zones_.PartitionRoots()) {
        if (const auto* found = FindPartitionNode(root.get(), zone_id)) {
            return found;
        }
    }
    return nullptr;
}

MergeRecommendation WorldRuntime::ScoreMergeWith(
    ZoneId parent_id,
    const std::shared_ptr<const LoadGrid>& field,
    const std::shared_ptr<const ActivityGrid>& activity,
    std::chrono::steady_clock::time_point now) const
{
    const ZonePartition* parent = FindZoneNode(parent_id);
    if (parent == nullptr || parent->children.size() != 4) {
        return MergeRecommendation{};
    }
    MergeScoreInput input;
    input.parent_id = parent_id;
    input.parent_bounds = parent->bounds;
    input.parent_depth = parent->depth;
    input.last_split = parent->last_split_time;
    input.last_merge = parent->last_merge_time;
    if (parent->group_low_since != std::chrono::steady_clock::time_point{}) {
        input.sustained_low_seconds =
            std::chrono::duration<float>(now - parent->group_low_since).count();
    }
    float max_child_load = 0.0f;
    for (std::size_t i = 0; i < parent->children.size(); ++i) {
        const ZonePartition* child = parent->children[i].get();
        input.child_ids[i] = child->zone_id;
        input.child_bounds[i] = child->bounds;
        max_child_load = std::max(max_child_load, child->load_score);
        const std::size_t zone_index = zones_.FindIndexById(child->zone_id);
        if (zone_index < zones_.ZoneCount()) {
            const auto& diag = zones_.GetZone(zone_index).Diagnostics();
            input.players += diag.player_count.load(std::memory_order_relaxed);
            input.mobs += diag.mob_count.load(std::memory_order_relaxed);
        }
    }
    input.max_child_load_score = max_child_load;
    MergeRecommendation recommendation =
        scorer_.ScoreMerge(input, field.get(), activity.get(), now);
    recommendation.activity_epoch = activity != nullptr ? activity->epoch : 0;
    return recommendation;
}

MergeRecommendation WorldRuntime::ScoreMerge(ZoneId parent_id) const
{
    return ScoreMergeWith(parent_id, load_field_.Snapshot(), activity_field_.Snapshot(),
                          std::chrono::steady_clock::now());
}

void WorldRuntime::RecordMergeDecision(const MergeRecommendation& recommendation,
                                       PartitionNoopReason reason,
                                       const char* detail,
                                       bool executed,
                                       std::chrono::steady_clock::time_point now)
{
    PartitionDecisionRecord record;
    record.kind = PartitionDecisionKind::Merge;
    record.timestamp = now;
    record.zone_id = recommendation.parent_id;
    record.executed = executed;
    record.scored = true;
    record.noop_reason = reason;
    record.detail = detail != nullptr ? detail : "";
    record.field_epoch = recommendation.field_epoch;
    record.expected_improvement = recommendation.expected_improvement;
    record.merge_candidate = recommendation.best;
    // The parent has no single tick: report the worst child signal so the
    // record still carries a comparable pressure reading.
    float max_p99 = 0.0f;
    float max_load = 0.0f;
    float max_field = 0.0f;
    for (const ZoneId child_id : recommendation.best.child_ids) {
        for (const auto& snap : load_monitor_.RecentSnapshots()) {
            if (snap.zone_id == child_id) {
                max_p99 = std::max(max_p99, snap.p99_tick_us / 1000.0f);
                max_load = std::max(max_load, snap.load_score);
                max_field = std::max(max_field, snap.field_load_score);
                break;
            }
        }
    }
    record.p99_tick_ms = max_p99;
    record.load_score = max_load;
    record.field_load_score = max_field;
    RecordPartitionDecision(std::move(record), executed, !executed);
}

void WorldRuntime::RecordMergeGateNoop(const MergeGroupSnapshot& group,
                                       std::chrono::steady_clock::time_point now)
{
    PartitionDecisionRecord record;
    record.kind = PartitionDecisionKind::Merge;
    record.timestamp = now;
    record.zone_id = group.parent_id;
    record.scored = false;
    record.detail = ZoneScheduler::MergeGateName(group.gate);
    record.merge_candidate.parent_id = group.parent_id;
    record.merge_candidate.child_ids = group.child_ids;
    record.merge_candidate.sustained_low_seconds = group.sustained_low_seconds;
    record.merge_candidate.predicted_parent_load = group.group_load_score;
    switch (group.gate) {
    case ZoneScheduler::MergeGate::SplitToMergeCooldown:
        record.noop_reason = PartitionNoopReason::MergeRecentSplit;
        break;
    case ZoneScheduler::MergeGate::MergeCooldown:
        record.noop_reason = PartitionNoopReason::MergeRecentMerge;
        break;
    case ZoneScheduler::MergeGate::NotSustained:
        record.noop_reason = PartitionNoopReason::MergeNotSustained;
        break;
    default:
        record.noop_reason = PartitionNoopReason::MergeNotEligible;
        break;
    }
    RecordPartitionDecision(std::move(record), false, true);
}

void WorldRuntime::NoteOscillationIfAny(const ZonePartition* node,
                                        bool split_committed,
                                        std::chrono::steady_clock::time_point now)
{
    if (node == nullptr) {
        return;
    }
    const float window_s = scorer_.GetConfig().oscillation_window_s;
    if (!(window_s > 0.0f)) {
        return;
    }
    const auto window = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(window_s));
    if (split_committed) {
        if (node->last_merge_time != std::chrono::steady_clock::time_point{} &&
            now - node->last_merge_time < window) {
            partition_metrics_.oscillation_guard_trips.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        if (node->last_split_time != std::chrono::steady_clock::time_point{} &&
            now - node->last_split_time < window) {
            partition_metrics_.oscillation_guard_trips.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void WorldRuntime::RecordPartitionDecision(PartitionDecisionRecord record,
                                           bool executed,
                                           bool why_not)
{
    (void)executed; // the record already carries the outcome
    std::lock_guard lock(decision_mutex_);
    const PartitionScoringConfig& config = scorer_.GetConfig();
    bool log_line = config.decision_log_enabled;
    if (log_line && why_not) {
        // Rate-limit why-not lines per zone: a stuck overloaded zone must not
        // log every control cycle (§30: decisions only, not per-tick spam).
        const auto interval = std::chrono::milliseconds(
            static_cast<std::int64_t>(std::max(0.0f, config.why_not_log_seconds) * 1000.0f));
        const auto it = why_not_last_logged_.find(record.zone_id);
        const bool due =
            it == why_not_last_logged_.end() || (record.timestamp - it->second) >= interval;
        if (due) {
            why_not_last_logged_[record.zone_id] = record.timestamp;
        } else {
            log_line = false;
        }
    }
    if (log_line) {
        LOG_INFO("partition decision: {}", FormatPartitionDecision(record));
    }
    constexpr std::size_t kDecisionCapacity = 64;
    if (decisions_.size() >= kDecisionCapacity) {
        decisions_.erase(decisions_.begin());
    }
    decisions_.push_back(std::move(record));
}

std::vector<PartitionDecisionRecord> WorldRuntime::PartitionDecisionLog() const
{
    std::lock_guard lock(decision_mutex_);
    return decisions_;
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
    RunSplitTransaction(zone_id, true, nullptr);
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

bool WorldRuntime::RunSplitTransaction(ZoneId zone_id, bool forced, const SplitCenter* center)
{
    const auto t_plan0 = std::chrono::steady_clock::now();
    ZoneManager::SplitPlan plan;
    if (!zones_.PlanSplit(zone_id, plan, nullptr, center)) {
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
    // Load field attribution: split/merge internal transfers are real
    // ownership work; counted at the destination like a migration.
    target_zone.LoadBins().NoteMigration(transfer.position.x, transfer.position.y);
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
    monitor.field_timescale = e.scoring.decision_timescale;
    load_monitor_ = ZoneLoadMonitor(monitor); // resets sustained timers; call pre-Start or idle

    // Adaptive scoring config: the scorer is read-only, the monitor's field
    // overload signal uses the same timescale so observe and score agree.
    // The geometric floor mirrors the effective (validated) partition floor.
    PartitionScoringConfig scoring = e.scoring;
    scoring.min_zone_size_m = e.min_zone_size_m;
    scoring.split_load_threshold = e.split_load_threshold; // post-merge ceiling mirror
    scorer_.SetConfig(scoring);

    scheduler_.config.split_load_threshold = e.split_load_threshold;
    scheduler_.config.merge_load_threshold = e.merge_load_threshold;
    scheduler_.config.sustained_window = std::chrono::seconds(e.sustained_window_seconds);
    scheduler_.config.split_cooldown = std::chrono::seconds(e.split_cooldown_seconds);
    scheduler_.config.merge_cooldown = std::chrono::seconds(e.merge_cooldown_seconds);
    scheduler_.config.max_depth = static_cast<std::uint8_t>(e.max_partition_depth);
    // Phase-3 stability: directional cooldowns, group sustained-low window and
    // the measured emergency bypass seam.
    scheduler_.config.split_to_merge_cooldown =
        std::chrono::seconds(static_cast<int>(e.scoring.split_to_merge_cooldown_s));
    scheduler_.config.merge_to_split_cooldown =
        std::chrono::seconds(static_cast<int>(e.scoring.merge_to_split_cooldown_s));
    scheduler_.config.merge_sustained_low =
        std::chrono::seconds(static_cast<int>(e.scoring.merge_sustained_low_s));
    scheduler_.config.emergency_split_bypass = e.scoring.emergency_split_bypass;
    scheduler_.config.emergency_p99_multiplier = e.scoring.emergency_p99_multiplier;
    scheduler_.config.tick_budget_ms = e.tick_budget_ms;

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
    const auto& s = e.scoring;
    LOG_INFO("partition scoring effective: min_improvement={:.2f} band={:.0f}m hotspot>={:.2f}x{} "
             "weights=[bal={:.2f} bnd={:.2f} mig={:.2f} rep={:.2f} inst={:.2f} topo={:.2f}] "
             "budgets=[act={:.0f} mig={:.1f} rep={:.1f} cbt={:.1f} work={:.0f}] "
             "timescale={} log={} why_not_interval={:.0f}s",
             s.min_expected_improvement,
             s.boundary_band_m,
             s.hotspot_threshold,
             s.hotspot_max_count,
             s.weight_balance,
             s.weight_boundary,
             s.weight_migration,
             s.weight_replication,
             s.weight_instability,
             s.topology_penalty,
             s.activity_band_budget,
             s.migration_band_budget,
             s.replication_band_budget,
             s.combat_band_budget,
             s.migration_work_budget,
             LoadTimescaleName(s.decision_timescale),
             s.decision_log_enabled ? "on" : "off",
             s.why_not_log_seconds);
    LOG_INFO("partition stability effective: merge_sustained_low={:.0f}s split_to_merge={:.0f}s "
             "merge_to_split={:.0f}s safety_margin={:.2f} min_merge_improvement={:.2f} "
             "merge_weights=[topo={:.2f} bnd={:.2f} mig={:.2f} rep={:.2f} risk={:.2f} exec={:.2f} "
             "inst={:.2f}] topology_benefit={:.2f} emergency=[bypass={} p99x{:.1f}] "
             "oscillation_window={:.0f}s",
             s.merge_sustained_low_s,
             s.split_to_merge_cooldown_s,
             s.merge_to_split_cooldown_s,
             s.post_merge_safety_margin,
             s.min_merge_improvement,
             s.weight_merge_topology,
             s.weight_merge_boundary,
             s.weight_merge_migration,
             s.weight_merge_replication,
             s.weight_merge_risk,
             s.weight_merge_execution,
             s.weight_merge_instability,
             s.merge_topology_benefit,
             s.emergency_split_bypass ? "on" : "off",
             s.emergency_p99_multiplier,
             s.oscillation_window_s);
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

void WorldRuntime::ConfigureLoadField(const LoadFieldConfig& config)
{
    // World geometry is runtime-owned: operator config never moves the world.
    LoadFieldConfig requested = config;
    requested.bounds = WorldBounds::FromExtent(terrain_.WorldExtentMeters());
    const auto validated = ValidateLoadFieldConfig(requested);
    for (const auto& warning : validated.warnings) {
        LOG_WARN("{}", warning);
    }
    effective_load_field_config_ = validated.effective;
    load_field_.Reconfigure(validated.effective);
    // Rebinds every zone's local load-bin rectangle to the effective grid. A
    // disabled field disables the bins too, so the hot path costs exactly
    // nothing (CellFor returns nullptr before any index math).
    // Documented precondition: pre-Start or idle (never with a tick in flight).
    LoadFieldMapping mapping = LoadFieldMapping::FromConfig(validated.effective);
    mapping.valid = validated.effective.enabled;
    zones_.ApplyLoadFieldMapping(mapping);
    last_load_field_build_ = std::chrono::steady_clock::now();
    const auto& e = validated.effective;
    LOG_INFO("load field effective: enabled={} cell={:.0f}m hz={:.2f} l1={}x{} "
             "budgets/s=[sim={:.0f} repl={:.0f} aoi={:.0f} combat={:.0f} mig={:.0f}] "
             "weights=[{:.2f}/{:.2f}/{:.2f}/{:.2f}/{:.2f}] taus=[{:.1f}/{:.1f}/{:.1f}/{:.1f}]",
             e.enabled,
             e.cell_size_m,
             e.aggregation_hz,
             e.l1_enabled ? e.l1_ratio : 0,
             e.l1_enabled ? e.l1_ratio : 0,
             e.simulation_budget,
             e.replication_budget,
             e.aoi_budget,
             e.combat_budget,
             e.migration_budget,
             e.weight_simulation,
             e.weight_replication,
             e.weight_aoi,
             e.weight_combat,
             e.weight_migration,
             e.fast_rise_tau_s,
             e.fast_fall_tau_s,
             e.slow_rise_tau_s,
             e.slow_fall_tau_s);
}

void WorldRuntime::RequestLoadFieldValidation()
{
    load_field_validation_requested_.store(true, std::memory_order_relaxed);
}

bool WorldRuntime::TryTakeLoadFieldValidationResult(std::string& out_result)
{
    std::lock_guard lock(load_field_validation_mutex_);
    if (!load_field_validation_ready_) {
        return false;
    }
    out_result = std::move(load_field_validation_result_);
    load_field_validation_result_.clear();
    load_field_validation_ready_ = false;
    return true;
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
