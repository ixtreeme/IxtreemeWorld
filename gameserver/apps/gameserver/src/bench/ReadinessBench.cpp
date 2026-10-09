#include "ReadinessBench.h"
#include "../world/activity/WakeCaptureProfile.h"

#include <algorithm>
#include <numeric>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "network/Session.h"
#include "map/WorldPackageWriter.h"

#include "../world/WorldRuntime.h"
#include "../world/debug/WorldValidator.h"
#include "../world/partition/PartitionScoring.h"
#include "../world/spawn/SpawnLoader.h"
#include "BenchSnapshot.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // PROCESS_MEMORY_COUNTERS (K32GetProcessMemoryInfo is in kernel32)
#endif

namespace gs::bench {

// --- environment / memory ---------------------------------------------------

std::size_t ProcessWorkingSetBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<std::size_t>(counters.WorkingSetSize);
    }
    return 0;
#else
    std::ifstream statm("/proc/self/statm");
    std::size_t total_pages = 0;
    std::size_t resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        return resident_pages * 4096u;
    }
    return 0;
#endif
}

namespace {

using Clock = std::chrono::steady_clock;

void PrintEnvironment()
{
#if defined(_WIN32)
    SYSTEM_INFO sys_info{};
    GetSystemInfo(&sys_info);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    std::printf("READINESS environment: os=windows cores=%u ram_gb=%.1f\n",
                static_cast<unsigned>(sys_info.dwNumberOfProcessors),
                static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
#else
    std::printf("READINESS environment: os=posix cores=%u\n", std::thread::hardware_concurrency());
#endif
#if defined(_MSC_VER)
    std::printf("READINESS build: compiler=msvc_%d ndebug=%d\n",
                _MSC_VER,
#if defined(NDEBUG)
                1
#else
                0
#endif
    );
#else
    std::printf("READINESS build: compiler=other ndebug=%d\n",
#if defined(NDEBUG)
                1
#else
                0
#endif
    );
#endif
}

double PercentileMs(std::vector<std::uint64_t>& samples, double percentile)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min(
        samples.size() - 1, static_cast<std::size_t>(std::floor(percentile * samples.size())));
    return static_cast<double>(samples[index]) / 1000.0;
}

gs::db::Character MakeReadinessCharacter(int index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{static_cast<std::uint64_t>(10000 + index)};
    character.account_id = gs::db::AccountId{static_cast<std::uint64_t>(20000 + index)};
    character.slot = 0;
    character.name = "Ready" + std::to_string(index);
    character.level = 1;
    character.class_id = 1;
    character.pos_x = 0;
    character.pos_y = 0;
    character.map_id = 0;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// --- window delta tracking --------------------------------------------------
// The supervisor exchanges per-zone diagnostic windows every second; sampling
// faster than that reconstructs the totals. On a drop (window exchanged and
// the counter reset to zero) the window's value was already accumulated
// incrementally up to `last`, so nothing may be added again -- the previous
// "add last" logic double-counted every window (up to ~2x totals, which made
// e.g. aoi_queries exceed the theoretical viewer*tick maximum). The tail
// between the last sample and the reset (<= sample interval) is not observed;
// with the 200 ms cadence that is at most ~20% of a 1 s window.
struct WindowDelta {
    std::uint64_t last = 0;
    std::uint64_t total = 0;
};

void AccumulateWindow(std::uint64_t current, WindowDelta& delta)
{
    if (current >= delta.last) {
        delta.total += current - delta.last;
    }
    delta.last = current;
}

struct ZoneStageTotals {
    WindowDelta tick_micros; // full-window tick time (ring covers only ~12.8s)
    WindowDelta gameplay;
    WindowDelta ai;
    WindowDelta movement;
    WindowDelta aoi;
    WindowDelta ghost;
    WindowDelta activity_publish;
    WindowDelta load_publish;
    WindowDelta replication;
    WindowDelta lod_eval;
    WindowDelta aoi_queries;
    WindowDelta repl_records;
    WindowDelta migrations;
    WindowDelta ghost_entities;
    WindowDelta ghost_publish_micros;
    WindowDelta ghost_reconcile_micros;
    WindowDelta ghost_publish_updates;
    WindowDelta ghost_publish_adds;
    WindowDelta ghost_publish_removes;
    WindowDelta ghost_publish_refreshes;
    WindowDelta ghost_publish_skips;
    WindowDelta ghost_keep;
    WindowDelta ghost_add;
    WindowDelta ghost_remove;
    WindowDelta ghost_reconcile_skips;
    WindowDelta ghost_delta_reconciles;
    WindowDelta ghost_full_reconciles;
    WindowDelta ghost_full_fallbacks;
    WindowDelta ghost_candidates;
    WindowDelta ghost_spatial_queries;
    // Phase 5B AOI + replication.
    WindowDelta aoi_candidates_pre_cap;
    WindowDelta aoi_candidates_post_cap;
    WindowDelta aoi_visible_final;
    WindowDelta interest_enter;
    WindowDelta interest_leave;
    WindowDelta interest_keep;
    WindowDelta repl_spawn;
    WindowDelta repl_despawn;
    WindowDelta repl_update;
    WindowDelta repl_suppressed;
    WindowDelta repl_fanout_relationships;
    WindowDelta repl_frame_bytes;
    WindowDelta repl_payload_bytes;
    WindowDelta repl_refresh;
    WindowDelta repl_aoi_us;
    WindowDelta repl_reconcile_us;
    WindowDelta repl_encode_us;
    WindowDelta repl_send_us;
    WindowDelta repl_record_requests;
    WindowDelta repl_record_serializations;
    WindowDelta repl_despawn_cache_hits;
    WindowDelta repl_despawn_cache_misses;
    WindowDelta repl_bytes_generated;
    WindowDelta repl_bytes_copied;
    WindowDelta repl_wire_bytes;
    // Phase 5D AOI stage breakdown + spatial index maintenance.
    WindowDelta aoi_cells_visited;
    WindowDelta aoi_entries_visited;
    WindowDelta aoi_exact_checks;
    WindowDelta aoi_index_us;
    WindowDelta aoi_topk_us;
    WindowDelta grid_inserts;
    WindowDelta grid_removes;
    WindowDelta grid_moves_in_cell;
    WindowDelta grid_moves_cell;
    // Phase 6 v2.
    WindowDelta repl_v2_full_records;
    WindowDelta repl_v2_delta_records;
    WindowDelta repl_v2_deferred;
    WindowDelta repl_v2_starvation;
    WindowDelta repl_v2_budget_hits;
    WindowDelta repl_v2_critical;
    WindowDelta repl_v2_delta_bytes;
    WindowDelta repl_v2_max_defer;
    WindowDelta repl_v2_tier_critical;
    WindowDelta repl_v2_tier_near;
    WindowDelta repl_v2_tier_normal;
    WindowDelta repl_v2_tier_reduced;
};

struct GlobalCounters {
    std::uint64_t attacks = 0;
    std::uint64_t deaths = 0;
    std::uint64_t lod_ai = 0;
    std::uint64_t lod_mv = 0;
    std::uint64_t lod_prom = 0;
    std::uint64_t lod_dem = 0;
    std::uint64_t lod_wake = 0;
    std::uint64_t lod_eval_us = 0;
    std::uint64_t split_commits = 0;
    std::uint64_t split_aborts = 0;
    std::uint64_t merge_commits = 0;
    std::uint64_t merge_aborts = 0;
    std::uint64_t merge_eval = 0;
    std::uint64_t merge_sup_not_eligible = 0;
    std::uint64_t merge_sup_not_sustained = 0;
    std::uint64_t merge_sup_recent_split = 0;
    std::uint64_t merge_sup_recent_merge = 0;
    std::uint64_t merge_sup_unsafe = 0;
    std::uint64_t merge_sup_min_improvement = 0;
    std::uint64_t split_sup_merge_cooldown = 0;
    std::uint64_t split_emergency = 0;
    std::uint64_t oscillation = 0;
    std::uint64_t migrations_committed = 0;
    std::uint64_t mig_stale = 0;
    std::uint64_t mig_dup = 0;
    std::uint64_t mig_retry = 0;
    std::uint64_t mig_fail = 0;
    std::uint64_t routes_local = 0;
    std::uint64_t routes_remote = 0;
    std::uint64_t routes_unavail = 0;
    std::uint64_t routes_draining = 0;
    std::uint64_t control_us = 0;
    std::uint64_t observe_us = 0;
    std::uint64_t score_us = 0;
    std::uint64_t migration_us = 0;
};

struct LoadWindowTotals {
    std::uint64_t sim_work = 0;
    std::uint64_t repl_bytes = 0;
    std::uint64_t repl_records = 0;
    std::uint64_t repl_dirty = 0;
    std::uint64_t aoi_queries = 0;
    std::uint64_t aoi_candidates = 0;
    std::uint64_t combat_events = 0;
    std::uint64_t migration_events = 0;
};

struct BenchPlayer {
    std::shared_ptr<gs::network::Session> session;
    gs::common::SessionId session_id = 0;
    int character_index = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t sequence = 0;
};

// Realistic mob leash: spawn radius == wander radius in the production spawn
// path, so a benchmark that spreads 200k mobs must place many SINGLE-mob
// points with a small leash instead of one huge point (which would turn every
// mob into a cross-zone wanderer and fabricate migration/ghost churn).
inline constexpr float kMobWanderLeashMeters = 40.0f;

std::size_t CountPartitionNodes(const std::vector<std::unique_ptr<gs::game::ZonePartition>>& roots)
{
    std::function<std::size_t(const gs::game::ZonePartition*)> count =
        [&](const gs::game::ZonePartition* node) -> std::size_t {
        if (node == nullptr) {
            return 0;
        }
        std::size_t total = 1;
        for (const auto& child : node->children) {
            total += count(child.get());
        }
        return total;
    };
    std::size_t total = 0;
    for (const auto& root : roots) {
        total += count(root.get());
    }
    return total;
}

// MAP-0: everything the report reads about the world, copied out by ONE
// supervisor-side snapshot (same epoch / world tick): zone table, per-zone
// gauges and tick samples, reclamation state, interest sets and the process
// working set all belong to the same mutation point.
struct ZoneReportRow {
    gs::game::ZoneId id = 0;
    gs::game::PartitionState partition{};
    bool sim_enabled = false;
    bool sleeping = false;
    bool tick_in_flight = false;
    std::uint64_t tick_index = 0;
    std::uint64_t lod_full = 0;
    std::uint64_t lod_reduced = 0;
    std::uint64_t lod_low = 0;
    std::uint64_t lod_dormant = 0;
    std::uint64_t players = 0;
    std::uint64_t mobs = 0;
    std::uint64_t ghosts = 0;
    std::vector<std::uint64_t> tick_samples;
};

struct ReadinessReport {
    bool generation_ok=false,wake_ok=false;
    std::string generation_error,wake_error;
    std::uint64_t field_generation=0,wake_generation=0,wake_cut_ns=0,capture_ns=0;
    std::uint64_t epoch = 0;
    std::uint32_t world_tick = 0;
    std::size_t zone_slots = 0;
    std::size_t active_leaves = 0;
    std::size_t partition_nodes = 0;
    std::size_t owners = 0;
    std::size_t working_set_bytes = 0;
    gs::game::ZoneManager::ReclaimStats reclaim{};
    std::vector<ZoneReportRow> zones;
    // Visible NetId set per viewer (sorted), Leaf + simulating zones only.
    std::vector<std::vector<std::uint32_t>> viewer_sets;
};

ReadinessReport CollectReadinessReport(const WorldSnapshot& snap)
{
    ReadinessReport report;
    report.epoch = snap.epoch;
    report.field_generation=snap.activity?snap.activity->epoch:0;
    report.wake_generation=snap.wake_decision?snap.wake_decision->generation:0;
    report.wake_cut_ns=snap.wake_decision?snap.wake_decision->cut_steady_ns:0;
    report.capture_ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(snap.captured_at.time_since_epoch()).count());
    report.generation_ok=snap.activity && gs::game::ValidateActivityGeneration(snap.zones,*snap.activity,report.generation_error);
    report.wake_ok=gs::game::ValidateActivityWake(snap.zones,snap.wake_decision.get(),report.wake_error);
    report.world_tick = snap.world_tick;
    report.zone_slots = snap.zones.ZoneCount();
    report.active_leaves = snap.zones.GetActiveLeaves().size();
    report.partition_nodes = CountPartitionNodes(snap.zones.PartitionRoots());
    report.owners = snap.owners.size();
    report.working_set_bytes = ProcessWorkingSetBytes();
    report.reclaim = snap.reclaim;
    report.zones.reserve(report.zone_slots);
    std::vector<std::uint64_t> scratch(256, 0u);
    for (std::size_t zi = 0; zi < snap.zones.ZoneCount(); ++zi) {
        const auto& zone = snap.zones.GetZone(zi);
        const auto& diag = zone.Diagnostics();
        ZoneReportRow row;
        row.id = zone.Id();
        row.partition = zone.Partition();
        row.sim_enabled = zone.SimulationEnabled();
        row.sleeping = zone.Activity() == gs::game::ZoneActivity::Sleeping;
        row.tick_in_flight = zone.TickInProgress().load(std::memory_order_acquire);
        row.tick_index = zone.TickIndex();
        row.lod_full = diag.lod_full.load(std::memory_order_relaxed);
        row.lod_reduced = diag.lod_reduced.load(std::memory_order_relaxed);
        row.lod_low = diag.lod_low.load(std::memory_order_relaxed);
        row.lod_dormant = diag.lod_dormant.load(std::memory_order_relaxed);
        row.players = diag.player_count.load(std::memory_order_relaxed);
        row.mobs = diag.mob_count.load(std::memory_order_relaxed);
        row.ghosts = diag.ghost_count.load(std::memory_order_relaxed);
        const std::size_t count = diag.CopyTickSamples(scratch.data(), scratch.size());
        row.tick_samples.assign(scratch.begin(), scratch.begin() + count);
        if (row.sim_enabled && row.partition == gs::game::PartitionState::Leaf) {
            for (const auto& [viewer_net, binding] : zone.Players()) {
                (void)viewer_net;
                std::vector<std::uint32_t> nets;
                nets.reserve(binding.visible_net_versions.size());
                for (const auto& [net_id, version] : binding.visible_net_versions) {
                    (void)version;
                    nets.push_back(net_id);
                }
                std::sort(nets.begin(), nets.end());
                report.viewer_sets.push_back(std::move(nets));
            }
        }
        report.zones.push_back(std::move(row));
    }
    return report;
}

} // namespace

int RunReadinessBenchmark(boost::asio::io_context& io, const ReadinessConfig& config)
{
    // Unbuffered stdout: a crash during setup must not swallow the phase log.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("READINESS %s: PASS\n", name);
        } else {
            std::printf("READINESS %s: FAIL\n", name);
            ++failures;
        }
    };

    PrintEnvironment();
    // Explicit synthetic remains the default; file-backed runs state their
    // separate residency mode in every result.
    std::printf("READINESS world: mode=%s terrain=%s\n", config.file_world ? "file-backed" : "synthetic",
                config.file_world ? (config.eager_terrain ? "eager non-flat" : "streamed non-flat") : "flat");
    std::printf("READINESS config: scenario=%s world_km=%.0f zones=%dx%d players=%d mobs=%d "
                "warmup_s=%d measure_s=%d asf_off=%d loadfield_off=%d lod_off=%d seed=%u\n",
                config.scenario.c_str(),
                config.world_km,
                config.zones_x,
                config.zones_y,
                config.players,
                config.mobs,
                config.warmup_seconds,
                config.measure_seconds,
                config.asf_off ? 1 : 0,
                config.load_field_off ? 1 : 0,
                config.lod_off ? 1 : 0,
                config.seed);

    const float extent = config.world_km * 1000.0f;
    const int zones_x = std::max(1, config.zones_x);
    const int zones_y = std::max(1, config.zones_y);
    const std::size_t expected_zones = static_cast<std::size_t>(zones_x) * zones_y;

    // ---- SETUP ------------------------------------------------------------
    std::unique_ptr<gs::game::WorldRuntime> runtime;
    const auto cold_start = Clock::now();
    if (config.file_world) {
        mx::map::PackageWriteSpec spec;
        spec.world_id = "map4-readiness";
        spec.world_name = "MAP-4 file-backed readiness";
        spec.cell_size_m = 16;
        spec.size_cells_x = static_cast<std::uint32_t>(extent / 16);
        spec.chunk_size_cells = 64;
        spec.height_raw = [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::int32_t>(1000 * std::sin(x * 0.01) + 500 * std::cos(y * 0.02));
        };
        spec.logic.spawns = {{1, 0, {10, 10, 20, 20}}};
        spec.water_model = mx::map::WaterModel::None;
        spec.overwrite = true;
        const auto dir = std::filesystem::temp_directory_path() / "ixw_map4_readiness" /
            (std::to_string(config.world_km) + "km");
        const auto write = mx::map::WritePackage(dir, spec);
        if (!write.ok) { std::printf("READINESS fixture failure: %s\n", write.error.c_str()); return 1; }
        const auto load_start = Clock::now();
        gs::game::WorldLoadRequest request;
        request.package_root = dir;
        request.mob_types_config = IXTREEME_DEFAULT_MOB_TYPES_CONFIG;
        request.residency = config.eager_terrain ? mx::map::ResidencyMode::Eager : mx::map::ResidencyMode::Streaming;
        mx::map::PackageReport report;
        auto world = gs::game::LoadWorldPackage(request, report);
        if (!world) {
            for (const auto& issue : report.issues) std::printf("%s\n", issue.Format().c_str());
            return 1;
        }
        gs::game::PartitionLayout layout;
        layout.regions_x = static_cast<std::uint32_t>(zones_x);
        layout.regions_y = static_cast<std::uint32_t>(zones_y);
        gs::game::TerrainStreamingConfig streaming;
        streaming.budget_bytes = static_cast<std::size_t>(config.terrain_budget_mb) * 1024 * 1024;
        streaming.retain_seconds = 0.5;
        runtime = std::make_unique<gs::game::WorldRuntime>(io, gs::game::RuntimeIdentity{},
            std::move(*world), layout, streaming);
        std::printf("READINESS package: path=%s chunks=%zu bytes_read=%llu load_ms=%.2f fixture_and_load_s=%.2f "
                    "cache=application-cold OS-cache=uncontrolled\n", dir.string().c_str(), report.manifest.chunks.size(),
                    static_cast<unsigned long long>(report.bytes_read),
                    std::chrono::duration<double, std::milli>(Clock::now() - load_start).count(),
                    std::chrono::duration<double>(Clock::now() - cold_start).count());
    } else {
        runtime = std::make_unique<gs::game::WorldRuntime>(io, gs::game::RuntimeIdentity{},
            gs::game::WorldRuntime::SyntheticWorldConfig{extent, static_cast<std::uint32_t>(zones_x),
                static_cast<std::uint32_t>(zones_y), {}});
    }
    auto& sim = *runtime;
    check("zones-built", ReadWorld(sim, [](const WorldSnapshot& snap) {
              return snap.zones.ZoneCount();
          }) == expected_zones);

    gs::game::PartitionConfig partition;
    if (config.asf_off) {
        partition.scoring.adaptive_enabled = false;
    }
    if (config.scenario == "churn") {
        // Documented deterministic accelerated-control mode: the churn
        // scenario compresses the stability timers so a full
        // hot->split->cool->merge cycle fits inside a benchmark run. The
        // mechanism (gates, cooldowns, transactions) is identical.
        partition.split_load_threshold = 0.4f;
        partition.merge_load_threshold = 0.2f;
        partition.sustained_window_seconds = 2;
        partition.split_cooldown_seconds = 2;
        partition.merge_cooldown_seconds = 2;
        partition.scoring.merge_sustained_low_s = 5.0f;
        partition.scoring.split_to_merge_cooldown_s = 8.0f;
        partition.scoring.merge_to_split_cooldown_s = 8.0f;
        partition.scoring.min_expected_improvement = 0.05f;
        partition.scoring.min_merge_improvement = 0.05f;
        partition.scoring.merge_topology_benefit = 0.15f;
        partition.scoring.instability_window_s = 20.0f;
        partition.scoring.oscillation_window_s = 10.0f;
    }
    sim.ConfigurePartition(partition);
    {
        // Phase 5B replication/AOI configuration: A/B switches are
        // benchmark-only; defaults are the optimized production path.
        gs::game::ReplicationConfig replication;
        replication.dirty_enabled = !config.repl_full;
        replication.aoi_partial_cap = !config.aoi_full_sort;
        replication.aoi_reference_positions = config.aoi_reference_positions;
        replication.aoi_nth_element = !config.aoi_partial_sort;
        replication.v2_enabled = !config.repl_v1;
        replication.network_lod_enabled = !config.netlod_off;
        replication.budget_max_records = static_cast<std::uint32_t>(
            std::max(0, config.budget_records));
        if (config.resync_ticks > 0) {
            replication.resync_ticks = static_cast<std::uint32_t>(config.resync_ticks);
        }
        sim.ConfigureReplication(replication);
    }
    if (config.ghost_shadow) {
        // Correctness run: a validator-detected inconsistency must stay
        // visible (no auto-repair) so the equivalence proof is honest.
        sim.SetGhostAutoRepair(false);
    }
    if (config.replication_shadow) {
        // Correctness run: retain the canonical shared records so the shadow
        // validator can compare them against authority (phase 5C).
        sim.SetReplicationAudit(true);
    }

    if (config.workers > 0) {
        sim.ConfigureWorkers(static_cast<std::size_t>(config.workers));
    }
    if (config.lod_off) {
        gs::game::LodConfig lod;
        lod.enabled = false;
        sim.ConfigureSimulationLod(lod);
    }
    if (config.load_field_off) {
        gs::game::LoadFieldConfig load_field;
        load_field.enabled = false;
        sim.ConfigureLoadField(load_field);
    }

    // ---- deterministic workload definition --------------------------------
    const std::string& scenario = config.scenario;
    const float hot_x = extent * 0.3f;
    const float hot_y = extent * 0.3f;
    std::vector<std::pair<float, float>> player_positions;
    player_positions.reserve(static_cast<std::size_t>(config.players));

    // Mob placement: deterministic single-mob points. Each point's radius is
    // both the placement scatter and the wander leash (production semantics),
    // so it stays at kMobWanderLeashMeters for a realistic simulation.
    std::size_t mobs_point_counter = 0;
    auto add_mob_point = [&](float x, float y, float leash) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = x;
        point.y = y;
        point.count = 1;
        point.radius = leash;
        sim.AddMobSpawnPoint(point);
        ++mobs_point_counter;
    };
    auto add_mob_points_uniform = [&](int total, float x0, float y0, float x1, float y1,
                                      float leash) {
        if (total <= 0) {
            return;
        }
        const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(
                                         static_cast<double>(total)))));
        const int rows = (total + cols - 1) / cols;
        for (int i = 0; i < total; ++i) {
            const int gx = i % cols;
            const int gy = i / cols;
            const float fx = (static_cast<float>(gx) + 0.5f) / static_cast<float>(cols);
            const float fy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(rows);
            add_mob_point(x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy, leash);
        }
    };
    auto add_mob_points_disc = [&](int total, float cx, float cy, float disc_radius, float leash) {
        if (total <= 0) {
            return;
        }
        for (int i = 0; i < total; ++i) {
            const float t = static_cast<float>(i) * 2.39996323f;
            const float r = disc_radius * std::sqrt((static_cast<float>(i) + 0.5f) /
                                                    static_cast<float>(total));
            add_mob_point(cx + std::cos(t) * r, cy + std::sin(t) * r, leash);
        }
    };

    auto add_player_grid = [&](int total, int grid_x, int grid_y, float x0, float y0, float x1,
                               float y1) {
        if (total <= 0) {
            return;
        }
        const int gx_count = std::max(1, grid_x);
        const int gy_count = std::max(1, grid_y);
        for (int i = 0; i < total; ++i) {
            const int gx = i % gx_count;
            const int gy = (i / gx_count) % gy_count;
            const float fx = (static_cast<float>(gx) + 0.5f) / static_cast<float>(gx_count);
            const float fy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(gy_count);
            player_positions.emplace_back(x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy);
        }
    };
    auto add_player_disc = [&](int total, float cx, float cy, float radius) {
        for (int i = 0; i < total; ++i) {
            // Deterministic spiral-ish placement, no RNG.
            const float t = static_cast<float>(i) * 2.39996323f; // golden angle
            const float r = radius * std::sqrt((static_cast<float>(i) + 0.5f) /
                                               static_cast<float>(std::max(1, total)));
            player_positions.emplace_back(cx + std::cos(t) * r, cy + std::sin(t) * r);
        }
    };

    const float leash = kMobWanderLeashMeters;
    if (scenario == "spread" || scenario == "quiet") {
        add_mob_points_uniform(config.mobs, 0.0f, 0.0f, extent, extent, leash);
        add_player_grid(config.players, 25, 20, extent * 0.05f, extent * 0.05f, extent * 0.95f,
                        extent * 0.95f);
    } else if (scenario == "hotspot") {
        // Meaningful hotspot density: 20k mobs over a ~1.5 km disc
        // (~2.8k/km^2) plus 400 players inside ~1 km.
        const int hotspot_mobs = std::min(config.mobs / 10, 20000);
        add_mob_points_disc(hotspot_mobs, hot_x, hot_y, 1500.0f, leash);
        add_mob_points_uniform(config.mobs - hotspot_mobs, 0.0f, 0.0f, extent, extent, leash);
        const int hotspot_players = static_cast<int>(static_cast<float>(config.players) * 0.8f);
        add_player_disc(hotspot_players, hot_x, hot_y, 500.0f);
        add_player_grid(config.players - hotspot_players, 10, 10, extent * 0.5f, extent * 0.5f,
                        extent * 0.95f, extent * 0.95f);
    } else if (scenario == "multi") {
        // A: simulation-heavy (mobs, few players); B: replication-heavy
        // (players, few mobs); C: mixed. Densities stay meaningful
        // (~2-4k mobs/km^2), never degenerate.
        const float ax = extent * 0.25f, ay = extent * 0.25f;
        const float bx = extent * 0.75f, by = extent * 0.25f;
        const float cx = extent * 0.5f, cy = extent * 0.75f;
        const int mobs_a = std::min(config.mobs / 10, 20000);
        const int mobs_b = std::min(config.mobs / 100, 1000);
        const int mobs_c = std::min(config.mobs / 10, 20000);
        add_mob_points_disc(mobs_a, ax, ay, 1500.0f, leash);
        add_mob_points_disc(mobs_b, bx, by, 500.0f, leash);
        add_mob_points_disc(mobs_c, cx, cy, 1500.0f, leash);
        add_mob_points_uniform(config.mobs - mobs_a - mobs_b - mobs_c, 0.0f, 0.0f, extent, extent,
                               leash);
        const int players_a = config.players / 10;
        const int players_b = static_cast<int>(static_cast<float>(config.players) * 0.6f);
        const int players_c = config.players / 5;
        add_player_disc(players_a, ax, ay, 150.0f);
        add_player_disc(players_b, bx, by, 250.0f);
        add_player_disc(players_c, cx, cy, 200.0f);
        add_player_grid(config.players - players_a - players_b - players_c, 10, 10,
                        extent * 0.05f, extent * 0.05f, extent * 0.45f, extent * 0.45f);
    } else if ((scenario == "c-moving-v1" || scenario == "c-moving-v2")) {
        if(config.players!=500 || config.mobs!=200000 || config.world_km!=100 || !config.file_world || config.eager_terrain || config.terrain_budget_mb!=16) {
            std::printf("C-MOVING invalid fixed population/geometry/cache contract\n"); return failures+1;
        }
        add_mob_points_uniform(config.mobs,0.0f,0.0f,extent,extent,leash);
        for(int i=0;i<config.players;++i) player_positions.emplace_back((5+i%25+0.5f)*1024,(5+i/25+0.5f)*1024);
        std::printf("C-MOVING v1: 500 one-player cohorts; A chunks x=5..29 y=5..24; B x=60..84 y=60..79; centre; setup=A; measure=B,A,B at 0,10,20s; production despawn/spawn relocation\n");
    } else if (scenario == "moving") {
        const int hotspot_mobs = std::min(config.mobs / 10, 20000);
        add_mob_points_disc(hotspot_mobs, hot_x, hot_y, 2000.0f, leash);
        add_mob_points_uniform(config.mobs - hotspot_mobs, 0.0f, 0.0f, extent, extent, leash);
        add_player_disc(config.players, hot_x, hot_y, 400.0f);
    } else if (scenario == "border") {
        add_mob_points_uniform(config.mobs, 0.0f, 0.0f, extent, extent, leash);
        // Players sit just west of the x = extent/2 boundary and run east/west
        // across it during the measure phase.
        const float boundary_x = extent * 0.5f;
        for (int i = 0; i < config.players; ++i) {
            const float y = extent * 0.2f +
                            static_cast<float>(i) * (extent * 0.6f / config.players);
            const float x = config.file_world
                ? (i % 2 ? boundary_x : std::ceil(boundary_x / 1024.0f) * 1024.0f) - 2.0f
                : boundary_x - 50.0f - static_cast<float>(i % 5) * 8.0f;
            player_positions.emplace_back(x, y);
        }
    } else if (scenario == "combat") {
        // The combat mob cluster is registered FIRST so its net ids are
        // predictable (1,000,000 + i*10 + j) and every player has ten targets
        // inside melee range.
        for (int i = 0; i < config.players; ++i) {
            const float t = static_cast<float>(i) * 2.39996323f;
            const float r = 400.0f * std::sqrt((static_cast<float>(i) + 0.5f) /
                                               static_cast<float>(std::max(1, config.players)));
            const float px = hot_x + std::cos(t) * r;
            const float py = hot_y + std::sin(t) * r;
            player_positions.emplace_back(px, py);
            for (int j = 0; j < 10; ++j) {
                add_mob_point(px + 0.5f * static_cast<float>(j % 3),
                              py + 0.5f * static_cast<float>(j / 3), 0.5f);
            }
        }
        const int combat_mobs = config.players * 10;
        if (config.mobs > combat_mobs) {
            add_mob_points_uniform(config.mobs - combat_mobs, 0.0f, 0.0f, extent, extent, leash);
        }
    } else if (scenario == "replication") {
        // Replication-heavy: many players packed together (fanout) with a
        // moderate mob population around them (~4.4k mobs/km^2).
        const int repl_mobs = std::min(config.mobs / 40, 5000);
        add_mob_points_disc(repl_mobs, hot_x, hot_y, 600.0f, leash);
        add_mob_points_uniform(config.mobs - repl_mobs, 0.0f, 0.0f, extent, extent, leash);
        add_player_disc(config.players, hot_x, hot_y, 300.0f);
    } else if (scenario == "churn") {
        const int churn_mobs = std::min(config.mobs / 20, 10000);
        add_mob_points_disc(churn_mobs, hot_x, hot_y, 1000.0f, leash);
        add_mob_points_uniform(config.mobs - churn_mobs, 0.0f, 0.0f, extent, extent, leash);
        add_player_disc(config.players, hot_x, hot_y, 350.0f);
    } else if (scenario == "dense") {
        // Worst PRACTICAL density: 20k mobs + all players inside a 2x2 km
        // area (5k mobs/km^2, 125 players/km^2) -- dense enough to stress
        // AOI/simulation/replication without being a degenerate point.
        const int dense_mobs = std::min(config.mobs, 20000);
        add_mob_points_disc(dense_mobs, hot_x, hot_y, 1100.0f, leash);
        add_mob_points_uniform(config.mobs - dense_mobs, 0.0f, 0.0f, extent, extent, leash);
        add_player_disc(config.players, hot_x, hot_y, 1000.0f);
    } else {
        std::printf("READINESS unknown scenario '%s'\n", scenario.c_str());
        return 1;
    }

    const std::size_t registered_points = mobs_point_counter;

    const auto setup_start = Clock::now();
    const std::size_t spawned_mobs = sim.SpawnConfiguredMobsNow();
    const double mob_spawn_s = std::chrono::duration<double>(Clock::now() - setup_start).count();
    std::printf("READINESS setup: spawn_points=%zu mobs_requested=%d mobs_spawned=%zu "
                "mob_spawn_s=%.2f\n",
                registered_points,
                config.mobs,
                spawned_mobs,
                mob_spawn_s);
    check("mobs-spawned", spawned_mobs == static_cast<std::size_t>(config.mobs));

    sim.Start();

    // Explicit demand + readiness check prevents the debug-spawn fallback
    // from silently collapsing a spread workload into the default spawn.
    auto next_terrain_trace = Clock::now();
    gs::game::TerrainRequestHandle prepared_terrain;
    std::vector<gs::game::TerrainRequestHandle> operation_samples;
    operation_samples.reserve(65536);
    std::vector<std::uint64_t> terrain_ready_wait_us;
    std::uint64_t terrain_prepare_failures=0;
    struct PrepareObservation {std::uint64_t request_id=0,first_poll_ns=0,observed_ns=0,polls=0,sleep_requested_ns=0,sleep_actual_ns=0;};
    struct BatchObservation {std::uint64_t planned_ns=0,start_ns=0,request_begin_ns=0,request_end_ns=0,completed_ns=0,next_eligible_ns=0;};
    std::vector<PrepareObservation> prepare_observations;prepare_observations.reserve(65536);
    std::array<BatchObservation,3> batch_observations{};
    Clock::time_point tc4_measure_start{};
    const auto stamp=[](Clock::time_point t){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());};
    const auto observed_wait=[&](PrepareObservation& observation,std::chrono::milliseconds timeout,const std::function<bool()>& condition) {
        const auto deadline=Clock::now()+timeout;
        const auto poll=[&](){const auto t=stamp(Clock::now());if(!observation.polls)observation.first_poll_ns=t;++observation.polls;const bool done=condition();if(done && !observation.observed_ns)observation.observed_ns=stamp(Clock::now());return done;};
        while(Clock::now()<deadline) {
            if(poll())return true;
            const auto a=Clock::now();std::this_thread::sleep_for(std::chrono::milliseconds(20));
            observation.sleep_requested_ns+=20000000;observation.sleep_actual_ns+=stamp(Clock::now())-stamp(a);
        }
        return poll();
    };
    auto prepare_spawn = [&](float x, float y) {
        if (!config.file_world || config.eager_terrain) return true;
        prepared_terrain=sim.PrepareTerrain(x,y,1.0f,15.0);
        if(operation_samples.size()<65536) operation_samples.push_back(prepared_terrain);
        PrepareObservation observation;observation.request_id=prepared_terrain->id;
        if(scenario=="c-moving-v2") {
            observation.first_poll_ns=stamp(Clock::now());observation.polls=1;
            prepared_terrain->WaitUntilReadyOrTerminal();
            observation.observed_ns=stamp(Clock::now());
        } else observed_wait(observation,std::chrono::seconds(15), [&] {
            if (Clock::now() >= next_terrain_trace) {
                next_terrain_trace = Clock::now() + std::chrono::seconds(1);
                const auto s = sim.GetTerrainStats().streamer;
                std::printf("READINESS admission-trace: target=(%.2f,%.2f) generation=%llu "
                    "request=%llu age_ms=%.2f status=%u "
                    "resident=%zu waiting=%zu loading=%zu io_queued=%zu completed_pending=%zu "
                    "memory=[accounted=%zu pinned=%zu soft=%zu evictable=%zu retired=%zu inflight=%zu metadata=%zu] "
                    "oldest_ms=%.1f room_checks=%llu blocked=%llu pumps=%llu quiescent=%llu pump_us=%llu\n",
                    x,y, (unsigned long long)s.generation,(unsigned long long)prepared_terrain->id,
                    std::chrono::duration<double,std::milli>(Clock::now()-prepared_terrain->started).count(),
                    unsigned(prepared_terrain->Status()),s.resident,s.waiting,s.loading,s.io_queued,s.completion_pending,
                    s.accounted_bytes,s.pinned_bytes,s.soft_retained_bytes,s.evictable_bytes,s.retired_bytes,
                    s.in_flight_bytes,s.metadata_bytes,s.oldest_wait_ms,(unsigned long long)s.room_checks,
                    (unsigned long long)s.room_blocked,(unsigned long long)s.pump_calls,
                    (unsigned long long)s.quiescent_pumps,(unsigned long long)s.pump_us);
            }
            return prepared_terrain->Status()!=gs::game::TerrainRequestStatus::Pending;
        });
        if(prepare_observations.size()<65536)prepare_observations.push_back(observation);
        const bool ready=prepared_terrain->Status()==gs::game::TerrainRequestStatus::Ready;
        if(ready) terrain_ready_wait_us.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-prepared_terrain->started).count()));
        else ++terrain_prepare_failures;
        if (!ready) {
            const auto stats = sim.GetTerrainStats().streamer;
            std::printf("READINESS terrain-timeout: x=%.2f y=%.2f limit_s=15 resident=%zu "
                        "waiting=%zu loading=%zu failed=%zu accounted_mb=%.2f budget_mb=%.2f "
                        "admission_rejects=%llu\n", x, y, stats.resident, stats.waiting,
                        stats.loading, stats.failed, stats.accounted_bytes / 1048576.0,
                        stats.budget_bytes / 1048576.0,
                        static_cast<unsigned long long>(stats.admission_rejects));
        }
        return ready;
    };

    std::vector<BenchPlayer> players(static_cast<std::size_t>(config.players));
    const auto player_spawn_start = Clock::now();
    for (int i = 0; i < config.players; ++i) {
        auto& player = players[static_cast<std::size_t>(i)];
        player.session_id = static_cast<gs::common::SessionId>(100 + i);
        player.character_index = i;
        player.x = player_positions[static_cast<std::size_t>(i)].first;
        player.y = player_positions[static_cast<std::size_t>(i)].second;
        if (!prepare_spawn(player.x, player.y)) {
            check("spawn-terrain-ready", false);
            sim.Stop();
            return failures;
        }
        boost::asio::ip::tcp::socket socket(io);
        player.session = std::make_shared<gs::network::Session>(std::move(socket),
                                                                player.session_id);
        sim.PostSpawn(player.session,
                      MakeReadinessCharacter(player.character_index),
                      gs::game::DebugSpawnOverride{player.x, player.y}, prepared_terrain);
    }
    const auto owner_count = [&sim] {
        return ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.owners.size(); });
    };
    const bool players_populated = WaitFor(std::chrono::seconds(180), [&] {
        return owner_count() == static_cast<std::size_t>(config.players);
    });
    const double player_spawn_s =
        std::chrono::duration<double>(Clock::now() - player_spawn_start).count();
    std::printf("READINESS setup: players_spawned=%zu player_spawn_s=%.2f\n",
                owner_count(),
                player_spawn_s);
    check("players-spawned", players_populated);
    if (!players_populated) {
        sim.Stop();
        return failures + 1;
    }
    auto requested_positions_present = [&] {
        return ReadWorld(sim, [&](const WorldSnapshot& snap) {
            for (const auto& player : players) {
                auto it = snap.owners.find(player.session_id);
                if (it == snap.owners.end()) return false;
                const auto e = snap.zones.GetZone(it->second.zone_index).FindEntity(it->second.net_id);
                if (!e.is_valid() || !e.has<gs::game::Position>()) return false;
                const auto p = e.get<gs::game::Position>();
                if (std::abs(p.x - player.x) > 0.01f || std::abs(p.y - player.y) > 0.01f) return false;
                if((scenario=="c-moving-v1" || scenario=="c-moving-v2")) {
                    // Independent fixture oracle: cohort centres are exact 16m vertices.
                    // No terrain query, cache access, fallback or residency assumption here.
                    const auto vx=static_cast<std::uint32_t>(player.x/16.0f),vy=static_cast<std::uint32_t>(player.y/16.0f);
                    const double expected=static_cast<std::int32_t>(1000*std::sin(vx*0.01)+500*std::cos(vy*0.02))*0.01;
                    if(!std::isfinite(p.z) || std::abs(p.z-expected)>0.001) return false;
                }
            }
            return true;
        });
    };
    if (config.file_world) {
        const bool placed = WaitFor(std::chrono::seconds(10), requested_positions_present);
        check("requested-player-positions", placed);
        if (!placed) { sim.Stop(); return failures; }
    }

    // ---- WARMUP -----------------------------------------------------------
    auto report_terrain_window = [&](const char* phase, const auto& stats) {
        if(!config.file_world || config.eager_terrain) return;
        const auto ready_p99=terrain_ready_wait_us.empty() ? std::string("N/A") :
            std::to_string(PercentileMs(terrain_ready_wait_us,0.99));
        const auto ready_max=terrain_ready_wait_us.empty() ? std::string("N/A") :
            std::to_string(*std::max_element(terrain_ready_wait_us.begin(),terrain_ready_wait_us.end())/1000.0);
        std::printf("READINESS preparation-window: phase=%s ready_samples=%zu failures=%llu "
            "request_to_ready_p99_ms=%s max_ms=%s (consumer polling included; Consume counted separately)\n",
            phase,terrain_ready_wait_us.size(),(unsigned long long)terrain_prepare_failures,ready_p99.c_str(),ready_max.c_str());
        std::vector<std::uint64_t> registered,ready,consume,effect,handoff;
        std::size_t pending=0,failed=0;
        for(const auto& r:operation_samples) {
            if(auto v=r->registered_us.load()) registered.push_back(v);
            if(auto v=r->ready_us.load()) ready.push_back(v);
            if(auto v=r->consumed_us.load()) {consume.push_back(v); auto t=r->ready_us.load(); if(t && v>=t) handoff.push_back(v-t);}
            if(auto v=r->effect_us.load()) effect.push_back(v);
            const auto status=r->Status(); pending+=status==gs::game::TerrainRequestStatus::Pending;
            failed+=status!=gs::game::TerrainRequestStatus::Pending && status!=gs::game::TerrainRequestStatus::Ready && status!=gs::game::TerrainRequestStatus::Consumed;
        }
        for(std::size_t i=0;i<std::min<std::size_t>(16,operation_samples.size());++i) {
            const auto& r=operation_samples[i];
            std::printf("SL2 operation-trace: phase=%s id=%llu class=%u start_us=%llu registered_offset_us=%llu ready_offset_us=%llu consume_offset_us=%llu effect_offset_us=%llu terminal_offset_us=%llu status=%u\n",
                phase,(unsigned long long)r->id,unsigned(r->priority.load()),
                (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(r->started.time_since_epoch()).count(),
                (unsigned long long)r->registered_us.load(),(unsigned long long)r->ready_us.load(),(unsigned long long)r->consumed_us.load(),
                (unsigned long long)r->effect_us.load(),(unsigned long long)r->terminal_us.load(),unsigned(r->Status()));
        }
        auto operation_p99=[](auto& v){return v.empty()?std::string("N/A"):std::to_string(PercentileMs(v,0.99));};
        std::printf("SL2 operations: phase=%s requested=%zu registered=%zu ready=%zu consumed=%zu effect=%zu failed=%zu pending=%zu start_registered_p99_ms=%s start_ready_p99_ms=%s ready_consume_p99_ms=%s start_consume_p99_ms=%s start_effect_p99_ms=%s ingress_requested_lifetime=%llu ingress_rejected_lifetime=%llu\n",
            phase,operation_samples.size(),registered.size(),ready.size(),consume.size(),effect.size(),failed,pending,
            operation_p99(registered).c_str(),operation_p99(ready).c_str(),operation_p99(handoff).c_str(),operation_p99(consume).c_str(),operation_p99(effect).c_str(),
            (unsigned long long)stats.commands_requested,(unsigned long long)stats.ingress_rejected);
        const auto& s=stats.streamer;
        std::printf("SL2 window: phase=%s started_us=%llu captured_us=%llu unique_requested=%llu unique_loaded=%llu reloads=%llu short_reload_under_1s=%llu room_us_lifetime=%llu unpublish_us_lifetime=%llu reclaim_us_lifetime=%llu completion_us_lifetime=%llu admission_us_lifetime=%llu\n",phase,
            (unsigned long long)s.window_started_us,(unsigned long long)s.captured_us,(unsigned long long)s.unique_requested,(unsigned long long)s.unique_loaded,
            (unsigned long long)s.reloads,(unsigned long long)s.short_reloads,(unsigned long long)s.room_us,(unsigned long long)s.unpublish_us,(unsigned long long)s.reclaim_us,(unsigned long long)s.completion_us,(unsigned long long)s.admission_us);
        for(unsigned cls=0;cls<3;++cls) std::printf("SL2 class: phase=%s class=%u examined_lifetime=%llu admitted_lifetime=%llu completed_lifetime=%llu waiting=%llu\n",phase,cls,
            (unsigned long long)s.examined_by_class[cls],(unsigned long long)s.admitted_by_class[cls],(unsigned long long)s.completed_by_class[cls],(unsigned long long)s.waiting_by_class[cls]);
        for(std::size_t i=0;i<s.reload_trace_count;++i) {const auto& t=s.reload_trace[i];
            std::printf("SL2 reload: phase=%s key=%u generation=%llu eviction_us=%llu demand_us=%llu publish_us=%llu\n",phase,t.chunk,(unsigned long long)t.generation,(unsigned long long)t.evicted_us,(unsigned long long)t.requested_us,(unsigned long long)t.published_us);}

        const std::string p99=s.window_miss_samples ? std::to_string(s.window_miss_p99_ms) : "N/A";
        std::printf("READINESS terrain-window: phase=%s id=%llu completed_misses=%llu p99_upper_ms=%s "
            "initial_waiting=%zu initial_oldest_ms=%.2f remaining_waiting=%zu remaining_oldest_ms=%.2f "
            "requests=[accepted=%llu consumed=%llu cancelled=%llu timeout=%llu rejected=%llu pending=%zu ready=%zu] "
            "pressure_evictions=%llu ingress_unique_rejected=%llu retry_suppressed=%llu "
            "safepoints=%llu drain_wait_us=%llu pump_us=%llu\n",phase,(unsigned long long)s.window_id,
            (unsigned long long)s.window_miss_samples,p99.c_str(),s.window_initial_waiting,s.window_initial_oldest_ms,
            s.waiting,s.oldest_wait_ms,(unsigned long long)s.requests_accepted,(unsigned long long)s.requests_consumed,
            (unsigned long long)s.requests_cancelled,(unsigned long long)s.requests_timed_out,
            (unsigned long long)s.requests_rejected,s.requests_pending,s.requests_ready,
            (unsigned long long)s.pressure_evictions,(unsigned long long)s.ingress_unique_rejected,
            (unsigned long long)s.retry_suppressed,(unsigned long long)stats.safepoints,
            (unsigned long long)stats.drain_wait_us,(unsigned long long)s.pump_us);
    };
    auto begin_terrain_window = [&] {
        if(!config.file_world || config.eager_terrain) return true;
        terrain_ready_wait_us.clear();
        operation_samples.clear();
        prepare_observations.clear();
        terrain_prepare_failures=0;
        const auto previous=sim.GetTerrainStats().streamer.window_id;
        sim.BeginTerrainMeasurementWindow();
        return WaitFor(std::chrono::seconds(5),[&]{return sim.GetTerrainStats().streamer.window_id>previous;});
    };
    report_terrain_window("setup",sim.GetTerrainStats());
    if(!begin_terrain_window()) {check("terrain-window-reset",false); sim.Stop(); return failures;}
    std::printf("READINESS warmup: %ds (fields, LOD, partition state settle)\n",
                config.warmup_seconds);
    const auto warmup_end = Clock::now() + std::chrono::seconds(config.warmup_seconds);
    while (Clock::now() < warmup_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    report_terrain_window("warmup",sim.GetTerrainStats());

    // ---- MEASURE ----------------------------------------------------------
    // Keyed by ZoneId, not slot: the partition controller may split/merge
    // during the measurement and H9 reuses retired slots for new zones, so a
    // slot index does not identify a zone across samples. The accumulation
    // runs inside the snapshot collector (supervisor, quiescent window) while
    // this thread is blocked in ReadWorld, so zone_totals is only ever touched
    // by one thread at a time (the future hands it back).
    std::unordered_map<gs::game::ZoneId, ZoneStageTotals> zone_totals;
    auto sample_zones = [&]() {
        ReadWorld(sim, [&zone_totals](const WorldSnapshot& snap) {
        for (std::size_t zi = 0; zi < snap.zones.ZoneCount(); ++zi) {
            const auto& zone = snap.zones.GetZone(zi);
            const auto& diag = zone.Diagnostics();
            auto& totals = zone_totals[zone.Id()];
            AccumulateWindow(diag.tick_micros_since_diag.load(std::memory_order_relaxed),
                            totals.tick_micros);
            AccumulateWindow(diag.gameplay_micros_since_diag.load(std::memory_order_relaxed),
                            totals.gameplay);
            AccumulateWindow(diag.ai_micros_since_diag.load(std::memory_order_relaxed), totals.ai);
            AccumulateWindow(diag.movement_micros_since_diag.load(std::memory_order_relaxed),
                            totals.movement);
            AccumulateWindow(diag.aoi_micros_since_diag.load(std::memory_order_relaxed),
                            totals.aoi);
            AccumulateWindow(diag.ghost_micros_since_diag.load(std::memory_order_relaxed),
                            totals.ghost);
            AccumulateWindow(
                diag.activity_publish_micros_since_diag.load(std::memory_order_relaxed),
                totals.activity_publish);
            AccumulateWindow(diag.load_publish_micros_since_diag.load(std::memory_order_relaxed),
                            totals.load_publish);
            AccumulateWindow(diag.replication_micros_since_diag.load(std::memory_order_relaxed),
                            totals.replication);
            AccumulateWindow(diag.lod_eval_us_since_diag.load(std::memory_order_relaxed),
                            totals.lod_eval);
            AccumulateWindow(diag.aoi_queries_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_queries);
            AccumulateWindow(diag.transform_records_since_diag.load(std::memory_order_relaxed),
                            totals.repl_records);
            AccumulateWindow(diag.migrations_since_diag.load(std::memory_order_relaxed),
                            totals.migrations);
            AccumulateWindow(diag.ghost_entities_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_entities);
            AccumulateWindow(diag.ghost_publish_micros_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_micros);
            AccumulateWindow(diag.ghost_reconcile_micros_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_reconcile_micros);
            AccumulateWindow(diag.ghost_publish_updates_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_updates);
            AccumulateWindow(diag.ghost_publish_adds_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_adds);
            AccumulateWindow(diag.ghost_publish_removes_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_removes);
            AccumulateWindow(diag.ghost_publish_refreshes_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_refreshes);
            AccumulateWindow(diag.ghost_publish_skips_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_publish_skips);
            AccumulateWindow(diag.ghost_keep_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_keep);
            AccumulateWindow(diag.ghost_add_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_add);
            AccumulateWindow(diag.ghost_remove_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_remove);
            AccumulateWindow(diag.ghost_reconcile_skips_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_reconcile_skips);
            AccumulateWindow(diag.ghost_delta_reconciles_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_delta_reconciles);
            AccumulateWindow(diag.ghost_full_reconciles_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_full_reconciles);
            AccumulateWindow(diag.ghost_full_fallbacks_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_full_fallbacks);
            AccumulateWindow(diag.ghost_candidates_examined_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_candidates);
            AccumulateWindow(diag.ghost_spatial_queries_since_diag.load(std::memory_order_relaxed),
                            totals.ghost_spatial_queries);
            AccumulateWindow(diag.aoi_candidates_pre_cap_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_candidates_pre_cap);
            AccumulateWindow(diag.aoi_candidates_post_cap_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_candidates_post_cap);
            AccumulateWindow(diag.aoi_visible_final_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_visible_final);
            AccumulateWindow(diag.interest_enter_since_diag.load(std::memory_order_relaxed),
                            totals.interest_enter);
            AccumulateWindow(diag.interest_leave_since_diag.load(std::memory_order_relaxed),
                            totals.interest_leave);
            AccumulateWindow(diag.interest_keep_since_diag.load(std::memory_order_relaxed),
                            totals.interest_keep);
            AccumulateWindow(diag.repl_spawn_since_diag.load(std::memory_order_relaxed),
                            totals.repl_spawn);
            AccumulateWindow(diag.repl_despawn_since_diag.load(std::memory_order_relaxed),
                            totals.repl_despawn);
            AccumulateWindow(diag.repl_update_since_diag.load(std::memory_order_relaxed),
                            totals.repl_update);
            AccumulateWindow(diag.repl_suppressed_since_diag.load(std::memory_order_relaxed),
                            totals.repl_suppressed);
            AccumulateWindow(diag.repl_fanout_relationships_since_diag.load(std::memory_order_relaxed),
                            totals.repl_fanout_relationships);
            AccumulateWindow(diag.repl_frame_bytes_since_diag.load(std::memory_order_relaxed),
                            totals.repl_frame_bytes);
            AccumulateWindow(diag.repl_payload_bytes_since_diag.load(std::memory_order_relaxed),
                            totals.repl_payload_bytes);
            AccumulateWindow(diag.repl_refresh_since_diag.load(std::memory_order_relaxed),
                            totals.repl_refresh);
            AccumulateWindow(diag.repl_aoi_us_since_diag.load(std::memory_order_relaxed),
                            totals.repl_aoi_us);
            AccumulateWindow(diag.repl_reconcile_us_since_diag.load(std::memory_order_relaxed),
                            totals.repl_reconcile_us);
            AccumulateWindow(diag.repl_encode_us_since_diag.load(std::memory_order_relaxed),
                            totals.repl_encode_us);
            AccumulateWindow(diag.repl_send_us_since_diag.load(std::memory_order_relaxed),
                            totals.repl_send_us);
            AccumulateWindow(diag.repl_record_requests_since_diag.load(std::memory_order_relaxed),
                            totals.repl_record_requests);
            AccumulateWindow(
                diag.repl_record_serializations_since_diag.load(std::memory_order_relaxed),
                totals.repl_record_serializations);
            AccumulateWindow(
                diag.repl_despawn_cache_hits_since_diag.load(std::memory_order_relaxed),
                totals.repl_despawn_cache_hits);
            AccumulateWindow(
                diag.repl_despawn_cache_misses_since_diag.load(std::memory_order_relaxed),
                totals.repl_despawn_cache_misses);
            AccumulateWindow(diag.repl_bytes_generated_since_diag.load(std::memory_order_relaxed),
                            totals.repl_bytes_generated);
            AccumulateWindow(diag.repl_bytes_copied_since_diag.load(std::memory_order_relaxed),
                            totals.repl_bytes_copied);
            AccumulateWindow(diag.repl_wire_bytes_since_diag.load(std::memory_order_relaxed),
                            totals.repl_wire_bytes);
            AccumulateWindow(diag.aoi_cells_visited_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_cells_visited);
            AccumulateWindow(diag.aoi_entries_visited_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_entries_visited);
            AccumulateWindow(diag.aoi_exact_checks_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_exact_checks);
            AccumulateWindow(diag.aoi_index_us_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_index_us);
            AccumulateWindow(diag.aoi_topk_us_since_diag.load(std::memory_order_relaxed),
                            totals.aoi_topk_us);
            AccumulateWindow(diag.grid_inserts_since_diag.load(std::memory_order_relaxed),
                            totals.grid_inserts);
            AccumulateWindow(diag.grid_removes_since_diag.load(std::memory_order_relaxed),
                            totals.grid_removes);
            AccumulateWindow(diag.grid_moves_in_cell_since_diag.load(std::memory_order_relaxed),
                            totals.grid_moves_in_cell);
            AccumulateWindow(diag.grid_moves_cell_since_diag.load(std::memory_order_relaxed),
                            totals.grid_moves_cell);
            AccumulateWindow(diag.repl_v2_full_records_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_full_records);
            AccumulateWindow(diag.repl_v2_delta_records_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_delta_records);
            AccumulateWindow(diag.repl_v2_deferred_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_deferred);
            AccumulateWindow(diag.repl_v2_starvation_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_starvation);
            AccumulateWindow(diag.repl_v2_budget_hits_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_budget_hits);
            AccumulateWindow(diag.repl_v2_critical_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_critical);
            AccumulateWindow(diag.repl_v2_delta_bytes_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_delta_bytes);
            // A max counter must not be summed across windows: keep the peak.
            {
                const std::uint64_t observed =
                    diag.repl_v2_max_defer_ticks.load(std::memory_order_relaxed);
                if (observed > totals.repl_v2_max_defer.total) {
                    totals.repl_v2_max_defer.total = observed;
                }
            }
            AccumulateWindow(diag.repl_v2_tier_critical_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_tier_critical);
            AccumulateWindow(diag.repl_v2_tier_near_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_tier_near);
            AccumulateWindow(diag.repl_v2_tier_normal_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_tier_normal);
            AccumulateWindow(diag.repl_v2_tier_reduced_since_diag.load(std::memory_order_relaxed),
                            totals.repl_v2_tier_reduced);
        }
        return snap.epoch;
        });
    };

    auto snapshot_globals = [&]() -> GlobalCounters {
        GlobalCounters counters;
        counters.attacks = sim.AttacksTotal();
        counters.deaths = sim.DeathsTotal();
        const auto lod = sim.LodWorkTotalsSnapshot();
        counters.lod_ai = lod.ai_updates;
        counters.lod_mv = lod.move_updates;
        counters.lod_prom = lod.promotions;
        counters.lod_dem = lod.demotions;
        counters.lod_wake = lod.wakes;
        counters.lod_eval_us = lod.eval_us;
        const auto part = sim.PartitionMetricsSnapshot();
        counters.split_commits = part.split_commits;
        counters.split_aborts = part.split_aborts;
        counters.merge_commits = part.merge_commits;
        counters.merge_aborts = part.merge_aborts;
        counters.merge_eval = part.merge_candidates_evaluated;
        counters.merge_sup_not_eligible = part.merge_suppressed_not_eligible;
        counters.merge_sup_not_sustained = part.merge_suppressed_not_sustained;
        counters.merge_sup_recent_split = part.merge_suppressed_recent_split;
        counters.merge_sup_recent_merge = part.merge_suppressed_recent_merge;
        counters.merge_sup_unsafe = part.merge_suppressed_post_merge_unsafe;
        counters.merge_sup_min_improvement = part.merge_suppressed_min_improvement;
        counters.split_sup_merge_cooldown = part.split_suppressed_merge_cooldown;
        counters.split_emergency = part.split_emergency_bypasses;
        counters.oscillation = part.oscillation_guard_trips;
        counters.control_us = part.control_us;
        counters.observe_us = part.observe_us;
        counters.score_us = part.score_us;
        counters.migration_us = part.migration_us;
        const auto mig = sim.MigrationMetrics();
        counters.migrations_committed = mig.committed;
        counters.mig_stale = mig.dropped_stale;
        counters.mig_dup = mig.duplicates;
        counters.mig_retry = mig.retries;
        counters.mig_fail = mig.failures;
        const auto routes = sim.Router().MetricsSnapshot();
        counters.routes_local = routes.local_delivered;
        counters.routes_remote = routes.remote_emulated;
        counters.routes_unavail = routes.unavailable;
        counters.routes_draining = routes.draining;
        return counters;
    };

    LoadWindowTotals load_window;
    std::uint64_t load_epoch = sim.LoadFieldMetrics().epoch;
    auto sample_load_field = [&]() {
        const auto metrics = sim.LoadFieldMetrics();
        if (metrics.epoch == load_epoch) {
            return;
        }
        load_epoch = metrics.epoch;
        load_window.sim_work += metrics.last_totals.sim_work;
        load_window.repl_bytes += metrics.last_totals.repl_bytes;
        load_window.repl_records += metrics.last_totals.repl_records;
        load_window.repl_dirty += metrics.last_totals.repl_dirty;
        load_window.aoi_queries += metrics.last_totals.aoi_queries;
        load_window.aoi_candidates += metrics.last_totals.aoi_candidates;
        load_window.combat_events += metrics.last_totals.combat_events;
        load_window.migration_events += metrics.last_totals.migration_events;
    };

    // Scenario actions.
    const float boundary_x = extent * 0.5f;
    int action_tick = 0;
    int relocation_step = 0;
    bool action_failed = false;
    auto run_actions = [&](Clock::time_point now) {
        static Clock::time_point next_action{};
        if (next_action == Clock::time_point{}) {
            next_action = now;
        }
        if (now < next_action) {
            return;
        }
        ++action_tick;
        if (scenario == "spread" || scenario == "dense") {
            // Deterministic walking so dirty transforms / movement are real.
            const float angle = 0.7f * static_cast<float>(action_tick);
            for (auto& player : players) {
                sim.PostMoveInput(player.session_id, ++player.sequence, angle,
                                  gs::game::MoveState::Walking);
            }
            next_action = now + std::chrono::seconds(2);
        } else if (scenario == "replication") {
            const float angle = 0.7f * static_cast<float>(action_tick);
            for (auto& player : players) {
                sim.PostMoveInput(player.session_id, ++player.sequence, angle,
                                  gs::game::MoveState::Walking);
            }
            next_action = now + std::chrono::seconds(1);
        } else if (scenario == "border") {
            const bool east = (action_tick % 2) == 1;
            const float angle = east ? 1.5707963f : -1.5707963f;
            for (auto& player : players) {
                sim.PostMoveInput(player.session_id, ++player.sequence, angle,
                                  gs::game::MoveState::Running);
            }
            next_action = now + std::chrono::seconds(3);
        } else if (scenario == "combat") {
            for (int i = 0; i < config.players; ++i) {
                for (int j = 0; j < 10; ++j) {
                    sim.PostAttackTarget(players[static_cast<std::size_t>(i)].session_id,
                                         static_cast<std::uint32_t>(1000000 + i * 10 + j));
                }
            }
            next_action = now + std::chrono::seconds(1);
        } else if (scenario == "moving" || scenario == "churn" || (scenario == "c-moving-v1" || scenario == "c-moving-v2")) {
            // Relocate the player hotspot (production despawn + spawn). The
            // mob population stays; player influence / AOI / replication move.
            const float path[3][2] = {
                {extent * 0.2f, extent * 0.3f}, {extent * 0.5f, extent * 0.5f},
                {extent * 0.8f, extent * 0.7f}};
            const int waypoint = relocation_step % 3;
            BatchObservation* batch=(scenario=="c-moving-v1" || scenario=="c-moving-v2") && relocation_step<3?&batch_observations[relocation_step]:nullptr;
            if(batch) {batch->planned_ns=stamp(tc4_measure_start+std::chrono::seconds(relocation_step*(config.measure_seconds/3)));batch->start_ns=stamp(now);}
            for (auto& player : players) {
                sim.PostDespawn(player.session_id);
            }
            if (!WaitFor(std::chrono::seconds(20), [&] { return owner_count() == 0; })) {
                check("relocation-despawn", false);
                action_failed = true;
                return;
            }
            if(batch)batch->request_begin_ns=stamp(Clock::now());
            for (auto& player : players) {
                const float t = static_cast<float>(player.character_index) * 2.39996323f;
                const float r = 400.0f * std::sqrt(
                    (static_cast<float>(player.character_index) + 0.5f) /
                    static_cast<float>(std::max(1, config.players)));
                player.x = path[waypoint][0] + std::cos(t) * r;
                player.y = path[waypoint][1] + std::sin(t) * r;
                if((scenario=="c-moving-v1" || scenario=="c-moving-v2")) {
                    const int origin=relocation_step%2==0 ? 60 : 5;
                    player.x=(origin+player.character_index%25+0.5f)*1024;
                    player.y=(origin+player.character_index/25+0.5f)*1024;
                }
                if (!prepare_spawn(player.x, player.y)) {
                    check("relocation-terrain-ready", false);
                    action_failed = true;
                    return; // one bounded failure, not N players * 15 seconds
                }
                boost::asio::ip::tcp::socket socket(io);
                player.session = std::make_shared<gs::network::Session>(std::move(socket),
                                                                        player.session_id);
                sim.PostSpawn(player.session,
                              MakeReadinessCharacter(player.character_index),
                              gs::game::DebugSpawnOverride{player.x, player.y}, prepared_terrain);
            }
            if(batch)batch->request_end_ns=stamp(Clock::now());
            ++relocation_step;
            if (config.file_world) check("requested-relocation-positions",
                WaitFor(std::chrono::seconds(10), requested_positions_present));
            next_action = now + std::chrono::seconds(
                                    scenario == "churn" ? 12 : config.measure_seconds / 3);
            if(batch){batch->completed_ns=stamp(Clock::now());batch->next_eligible_ns=stamp(next_action);}
        } else {
            next_action = now + std::chrono::seconds(2);
        }
    };

    std::printf("READINESS measure: %ds ghost_shadow=%d replication_shadow=%d "
                "repl_full=%d aoi_full_sort=%d aoi_reference_positions=%d\n",
                config.measure_seconds,
                config.ghost_shadow ? 1 : 0,
                config.replication_shadow ? 1 : 0,
                config.repl_full ? 1 : 0,
                config.aoi_full_sort ? 1 : 0,
                config.aoi_reference_positions ? 1 : 0);
    if(!begin_terrain_window()) {check("terrain-window-reset",false); sim.Stop(); return failures;}
    const auto measure_start = Clock::now();
    tc4_measure_start=measure_start;
    const auto terrain_start = sim.GetTerrainStats();
    const auto delays_start=sim.GetSchedulingDelays();
    const auto measure_end = measure_start + std::chrono::seconds(config.measure_seconds);
    auto next_sample = measure_start;
    auto next_ghost_shadow = measure_start;
    auto next_replication_shadow = measure_start;
    std::uint64_t ghost_shadow_runs = 0;
    std::uint64_t ghost_shadow_failures = 0;
    std::uint64_t replication_shadow_runs = 0;
    std::uint64_t replication_shadow_failures = 0;
    const auto sched_before = sim.SchedulerStats();
    const auto wake_before = sim.ActivityWakeMetrics();
    const auto profile_before=gs::game::wake_profile::Snapshot();
    const GlobalCounters counters_before = snapshot_globals();
    while (Clock::now() < measure_end) {
        const auto now = Clock::now();
        if (now >= next_sample) {
            sample_zones();
            sample_load_field();
            next_sample = now + std::chrono::milliseconds(200);
        }
        if (config.ghost_shadow) {
            if (now >= next_ghost_shadow) {
                sim.RequestGhostValidation();
                next_ghost_shadow = now + std::chrono::seconds(2);
            }
            std::string result;
            if (sim.TryTakeGhostValidationResult(result)) {
                ++ghost_shadow_runs;
                if (result != "OK") {
                    ++ghost_shadow_failures;
                    if (ghost_shadow_failures <= 3) {
                        std::printf("READINESS ghost-shadow FAIL: %s\n", result.c_str());
                    }
                }
            }
        }
        if (config.replication_shadow) {
            if (now >= next_replication_shadow) {
                sim.RequestReplicationValidation();
                next_replication_shadow = now + std::chrono::seconds(2);
            }
            std::string result;
            if (sim.TryTakeReplicationValidationResult(result)) {
                ++replication_shadow_runs;
                if (result != "OK") {
                    ++replication_shadow_failures;
                    if (replication_shadow_failures <= 3) {
                        std::printf("READINESS replication-shadow FAIL: %s\n", result.c_str());
                    }
                }
            }
        }
        run_actions(now);
        if (action_failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sample_zones();
    sample_load_field();
    const auto sched_after = sim.SchedulerStats();
    const GlobalCounters counters_after = snapshot_globals();
    const double measure_actual_s =
        std::chrono::duration<double>(Clock::now() - measure_start).count();
    // Freeze terrain counters with the measured interval, before the final
    // audit/report snapshots can add further I/O or wait time.
    const auto terrain_after_measure=sim.GetTerrainStats();
    const auto wake_after=sim.ActivityWakeMetrics();
    gs::game::wake_profile::Print("measure",profile_before,gs::game::wake_profile::Snapshot());
    std::printf("TC wake: input_generations=%llu consumed_updates=%llu capture_us=%llu commit_to_decision_sum_us=%llu max_lifetime_us=%llu (completed phase-input updates; coalesced oldest pending stamp; lifetime max includes setup)\n",
        (unsigned long long)(wake_after.inputs-wake_before.inputs),(unsigned long long)(wake_after.sources-wake_before.sources),
        (unsigned long long)(wake_after.capture_us-wake_before.capture_us),(unsigned long long)(wake_after.decision_age_us-wake_before.decision_age_us),
        (unsigned long long)wake_after.max_decision_age_us);
    if((scenario=="c-moving-v1" || scenario=="c-moving-v2")) {
        std::printf("TC4B window_start_ns=%llu cutoff_ns=%llu capture_ns=%llu planned_batches=3 started_batches=%d completed_batches=%d overflow=%d\n",stamp(measure_start),stamp(measure_end),stamp(Clock::now()),int(std::count_if(batch_observations.begin(),batch_observations.end(),[](const auto& b){return b.start_ns!=0;})),int(std::count_if(batch_observations.begin(),batch_observations.end(),[](const auto& b){return b.completed_ns!=0;})),int(operation_samples.size()>=65536));
        for(std::size_t i=0;i<batch_observations.size();++i) {
            const auto& b=batch_observations[i];
            std::printf("TC4BATCH id=%zu planned_ns=%llu start_ns=%llu request_begin_ns=%llu request_end_ns=%llu completed_ns=%llu next_eligible_ns=%llu reason=%s\n",i+1,b.planned_ns?b.planned_ns:stamp(measure_start+std::chrono::seconds(i*(config.measure_seconds/3))),b.start_ns,b.request_begin_ns,b.request_end_ns,b.completed_ns,b.next_eligible_ns,b.start_ns?"started":"outer-cutoff-before-next-start");
        }
        bool trace_coherent=operation_samples.size()==prepare_observations.size();
        for(std::size_t i=0;i<operation_samples.size() && i<prepare_observations.size();++i) {
            const auto& r=*operation_samples[i];const auto& o=prepare_observations[i];
            trace_coherent=trace_coherent && o.request_id==r.id && o.first_poll_ns>=stamp(r.started) && o.observed_ns>=o.first_poll_ns;
            std::printf("TC4REQUEST id=%llu batch=%zu player=%zu start_ns=%llu deadline_ns=%llu registered_us=%llu ready_us=%llu consume_us=%llu effect_us=%llu terminal_us=%llu status=%u first_poll_ns=%llu observed_ns=%llu polls=%llu sleep_requested_ns=%llu sleep_actual_ns=%llu\n",r.id,1+i/std::max(1,config.players),i%std::max(1,config.players),stamp(r.started),stamp(r.deadline),r.registered_us.load(),r.ready_us.load(),r.consumed_us.load(),r.effect_us.load(),r.terminal_us.load(),unsigned(r.Status()),o.first_poll_ns,o.observed_ns,o.polls,o.sleep_requested_ns,o.sleep_actual_ns);
        }
        check("tc4-request-trace-coherent",trace_coherent);
    }
    report_terrain_window("measure",terrain_after_measure);
    std::printf("SL2 supervisor: cpu_us=%llu drain_max_lifetime_us=%llu (GetThreadTimes on Windows; 0 on unsupported platforms is NOT MEASURED)\n",(unsigned long long)(terrain_after_measure.supervisor_cpu_us-terrain_start.supervisor_cpu_us),(unsigned long long)terrain_after_measure.drain_max_us);
    const auto delays_end=sim.GetSchedulingDelays();
    std::printf("SL2 scheduling: completed_samples=%llu due_enqueue_us=%llu queue_us=%llu due_start_us=%llu finish_after_due_plus_50ms=%llu (sum over zones; not a global pause)\n",
        (unsigned long long)(delays_end.samples-delays_start.samples),(unsigned long long)(delays_end.due_to_enqueue_us-delays_start.due_to_enqueue_us),
        (unsigned long long)(delays_end.queue_us-delays_start.queue_us),(unsigned long long)(delays_end.due_to_start_us-delays_start.due_to_start_us),
        (unsigned long long)(delays_end.finish_deadline_misses-delays_start.finish_deadline_misses));
    std::printf("SL2 movement: attempted=%llu waiting=%llu blocked=%llu invalid=%llu\n",
        (unsigned long long)(terrain_after_measure.queries.steps_attempted-terrain_start.queries.steps_attempted),
        (unsigned long long)(terrain_after_measure.queries.steps_waiting-terrain_start.queries.steps_waiting),
        (unsigned long long)(terrain_after_measure.queries.steps_blocked-terrain_start.queries.steps_blocked),
        (unsigned long long)(terrain_after_measure.queries.invalid-terrain_start.queries.invalid));
    const std::size_t zones_after_measure =
        ReadWorld(sim, [](const WorldSnapshot& snap) { return snap.zones.ZoneCount(); });

    // ---- Phase 7 scheduler audit line ------------------------------------
    {
        const auto delta = [](std::uint64_t after, std::uint64_t before) {
            return after >= before ? after - before : 0;
        };
        const std::uint64_t work = delta(sched_after.worker_work_micros,
                                         sched_before.worker_work_micros);
        const std::uint64_t idle = delta(sched_after.worker_idle_micros,
                                         sched_before.worker_idle_micros);
        const std::uint64_t busy_phase = delta(sched_after.busy_phase_micros,
                                               sched_before.busy_phase_micros);
        const std::uint64_t idle_phase = delta(sched_after.idle_phase_micros,
                                               sched_before.idle_phase_micros);
        const std::uint64_t sched_us = delta(sched_after.schedule_micros,
                                             sched_before.schedule_micros);
        const std::uint64_t enqueued = delta(sched_after.enqueued, sched_before.enqueued);
        const std::uint64_t due_zones = delta(sched_after.due_zones, sched_before.due_zones);
        const std::uint64_t waves = delta(sched_after.waves, sched_before.waves);
        const std::uint64_t cas_failures = delta(sched_after.cas_failures,
                                                 sched_before.cas_failures);
        const std::uint64_t sleeping = delta(sched_after.sleeping_skips,
                                             sched_before.sleeping_skips);
        std::uint64_t max_worker = 0;
        std::uint64_t min_worker = UINT64_MAX;
        for (std::size_t i = 0; i < sched_after.worker_work.size() &&
                               i < sched_before.worker_work.size();
             ++i) {
            const std::uint64_t w = delta(sched_after.worker_work[i],
                                          sched_before.worker_work[i]);
            max_worker = std::max(max_worker, w);
            min_worker = std::min(min_worker, w);
        }
        if (min_worker == UINT64_MAX) {
            min_worker = 0;
        }
        const double avg_worker =
            sched_after.workers > 0
                ? static_cast<double>(work) / static_cast<double>(sched_after.workers)
                : 0.0;
        const double imbalance = avg_worker > 0.0 ? static_cast<double>(max_worker) / avg_worker
                                                  : 0.0;
        // Effective parallelism: average busy workers over the measured wall
        // time (the supervisor's phase sampling is far coarser than a tick,
        // so deriving it from busy_phase would overstate it).
        const double parallelism =
            measure_actual_s > 0.0
                ? static_cast<double>(work) / (measure_actual_s * 1.0e6)
                : 0.0;
        std::printf("READINESS sched: workers=%zu active_zones=%zu parallelism=%.2f "
                    "imbalance=%.2f worker_work=[min=%llu avg=%.0f max=%llu us] idle_us=%llu "
                    "phase=[busy=%llu idle=%llu us] sched_us=%llu enqueued=%llu due=%llu "
                    "waves=%llu cas_failures=%llu sleeping=%llu\n",
                    sched_after.workers,
                    zones_after_measure,
                    parallelism,
                    imbalance,
                    (unsigned long long)min_worker,
                    avg_worker,
                    (unsigned long long)max_worker,
                    (unsigned long long)idle,
                    (unsigned long long)busy_phase,
                    (unsigned long long)idle_phase,
                    (unsigned long long)sched_us,
                    (unsigned long long)enqueued,
                    (unsigned long long)due_zones,
                    (unsigned long long)waves,
                    (unsigned long long)cas_failures,
                    (unsigned long long)sleeping);
    }

    // ---- FINAL VALIDATION -------------------------------------------------
    sim.RequestValidation();
    bool validated = false;
    std::string validation_result = "timeout";
    const auto validation_wait_start = Clock::now();
    // A quiescent window can be rare while hundreds of zones tick in flight;
    // wait up to 240s for the audit slot (the audit itself scans all
    // authority, which is why it only runs in this explicit debug window).
    for (int i = 0; i < 2400; ++i) {
        if (sim.TryTakeValidationResult(validation_result)) {
            validated = validation_result == "OK";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const double validation_wait_s =
        std::chrono::duration<double>(Clock::now() - validation_wait_start).count();
    // ---- REPORT SNAPSHOT --------------------------------------------------
    // One supervisor-side capture: every world-derived number below (zone
    // count, tick samples, tier gauges, residency, interest sets, slot reuse,
    // working set) comes from this single epoch.
    const auto report_wait_start = Clock::now();
    const ReadinessReport report = ReadWorld(sim, CollectReadinessReport);
    const double report_wait_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - report_wait_start).count();
    std::size_t stuck_zones = 0;
    std::size_t staging_zones = 0;
    std::uint64_t zone_ticks = 0;
    for (const auto& row : report.zones) {
        stuck_zones += row.tick_in_flight ? 1u : 0u;
        staging_zones += row.partition == gs::game::PartitionState::Staging ? 1u : 0u;
        zone_ticks += row.tick_index;
    }
    std::printf("READINESS validation_wait_s=%.1f zone_ticks=%llu\n",
                validation_wait_s,
                (unsigned long long)zone_ticks);
    std::printf("READINESS snapshot: epoch=%llu world_tick=%u wait_ms=%.1f slots=%zu "
                "active_leaves=%zu owners=%zu tick_in_flight=%zu staging=%zu "
                "reclaim=[pending=%zu reusable=%zu reclaimed=%llu reused=%llu trimmed=%llu]\n",
                (unsigned long long)report.epoch,
                report.world_tick,
                report_wait_ms,
                report.zone_slots,
                report.active_leaves,
                report.owners,
                stuck_zones,
                staging_zones,
                report.reclaim.retired_pending,
                report.reclaim.reusable,
                (unsigned long long)report.reclaim.reclaimed_total,
                (unsigned long long)report.reclaim.reused_total,
                (unsigned long long)report.reclaim.trimmed_total);
    check("validation", validated);
    check("activity-generation",report.generation_ok);
    check("activity-current-phase",report.wake_ok);
    std::printf("TC activity-audit: epoch=%llu world_tick=%u capture_ns=%llu field_generation=%llu wake_generation=%llu wake_cut_ns=%llu phase=quiescent-between-scheduling-phases generation_error=%s wake_error=%s\n",
        (unsigned long long)report.epoch,report.world_tick,(unsigned long long)report.capture_ns,
        (unsigned long long)report.field_generation,(unsigned long long)report.wake_generation,(unsigned long long)report.wake_cut_ns,
        report.generation_error.c_str(),report.wake_error.c_str());
    // A capture is served only in a quiescent window between two topology
    // transactions: no tick may be in flight and no staged split/merge
    // destination may exist in it.
    check("snapshot-quiescent", stuck_zones == 0 && staging_zones == 0);

    // ---- REPORT -----------------------------------------------------------
    std::vector<std::uint64_t> tick_samples;
    tick_samples.reserve(report.zones.size() * 64);
    gs::game::ZoneId worst_zone_id = 0;
    bool worst_zone_found = false;
    double worst_zone_p99 = 0.0;
    for (const auto& row : report.zones) {
        std::vector<std::uint64_t> local = row.tick_samples;
        if (!local.empty()) {
            const double local_p99 = PercentileMs(local, 0.99);
            if (local_p99 > worst_zone_p99) {
                worst_zone_p99 = local_p99;
                worst_zone_id = row.id;
                worst_zone_found = true;
            }
        }
        tick_samples.insert(tick_samples.end(), local.begin(), local.end());
    }
    const double tick_avg = [&] {
        if (tick_samples.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (const auto sample : tick_samples) {
            sum += static_cast<double>(sample);
        }
        return sum / static_cast<double>(tick_samples.size()) / 1000.0;
    }();
    const double tick_p50 = PercentileMs(tick_samples, 0.50);
    const double tick_p95 = PercentileMs(tick_samples, 0.95);
    const double tick_p99 = PercentileMs(tick_samples, 0.99);
    const double tick_max = PercentileMs(tick_samples, 1.0);

    std::uint64_t lod_full = 0;
    std::uint64_t lod_reduced = 0;
    std::uint64_t lod_low = 0;
    std::uint64_t lod_dormant = 0;
    std::uint64_t resident_players = 0;
    std::uint64_t resident_mobs = 0;
    std::uint64_t ghosts = 0;
    std::size_t active_zones = 0;
    std::size_t sleeping_zones = 0;
    for (const auto& row : report.zones) {
        // (The old live-read report skipped Staging destinations of a split
        // in flight; a snapshot never contains one -- checked above.)
        lod_full += row.lod_full;
        lod_reduced += row.lod_reduced;
        lod_low += row.lod_low;
        lod_dormant += row.lod_dormant;
        resident_players += row.players;
        resident_mobs += row.mobs;
        ghosts += row.ghosts;
        if (row.players > 0 || row.mobs > 0) {
            ++active_zones;
        }
        if (row.sleeping) {
            ++sleeping_zones;
        }
    }

    auto total_of = [&](WindowDelta ZoneStageTotals::*member) {
        std::uint64_t total = 0;
        for (const auto& [zone_id, zone] : zone_totals) {
            (void)zone_id;
            total += (zone.*member).total;
        }
        return total;
    };
    const std::uint64_t stage_gameplay = total_of(&ZoneStageTotals::gameplay);
    const std::uint64_t stage_ai = total_of(&ZoneStageTotals::ai);
    const std::uint64_t stage_movement = total_of(&ZoneStageTotals::movement);
    const std::uint64_t stage_aoi = total_of(&ZoneStageTotals::aoi);
    const std::uint64_t stage_ghost = total_of(&ZoneStageTotals::ghost);
    const std::uint64_t stage_activity = total_of(&ZoneStageTotals::activity_publish);
    const std::uint64_t stage_load_publish = total_of(&ZoneStageTotals::load_publish);
    const std::uint64_t stage_replication = total_of(&ZoneStageTotals::replication);
    const std::uint64_t stage_lod_eval = total_of(&ZoneStageTotals::lod_eval);
    const std::uint64_t total_aoi_queries = total_of(&ZoneStageTotals::aoi_queries);
    const std::uint64_t total_repl_records = total_of(&ZoneStageTotals::repl_records);
    const std::uint64_t total_migrations = total_of(&ZoneStageTotals::migrations);
    const std::uint64_t total_ghost_entities = total_of(&ZoneStageTotals::ghost_entities);
    const std::uint64_t stage_measured_total = stage_gameplay + stage_ghost + stage_replication +
                                               stage_lod_eval + stage_activity +
                                               stage_load_publish;

    const auto delta = [](std::uint64_t after, std::uint64_t before) {
        return after >= before ? after - before : after;
    };

    std::printf("READINESS tick: zones=%zu samples=%zu avg_ms=%.3f p50_ms=%.3f p95_ms=%.3f "
                "p99_ms=%.3f max_ms=%.3f\n",
                report.zone_slots,
                tick_samples.size(),
                tick_avg,
                tick_p50,
                tick_p95,
                tick_p99,
                tick_max);
    if (worst_zone_found) {
        std::printf("READINESS tick worst-zone: zone_id=%u p99_ms=%.3f\n",
                    worst_zone_id,
                    worst_zone_p99);
    }
    std::printf("READINESS tiers: full=%llu reduced=%llu low=%llu dormant=%llu (mobs=%llu)\n",
                (unsigned long long)lod_full,
                (unsigned long long)lod_reduced,
                (unsigned long long)lod_low,
                (unsigned long long)lod_dormant,
                (unsigned long long)resident_mobs);
    // Tolerance: the snapshot removes the read race, but the tier gauges
    // themselves are eventual-consistent (1 Hz recount + insert bumps during
    // transfers), so the sum may still differ slightly from the mob count at
    // one capture point. A systematic leak (stale retired/tombstoned gauges)
    // would exceed this.
    const std::uint64_t tier_sum = lod_full + lod_reduced + lod_low + lod_dormant;
    const std::uint64_t tier_diff =
        tier_sum > resident_mobs ? tier_sum - resident_mobs : resident_mobs - tier_sum;
    const std::uint64_t tier_tolerance = std::max<std::uint64_t>(32, resident_mobs / 500);
    if (tier_diff > tier_tolerance) {
        std::size_t shown = 0;
        for (const auto& row : report.zones) {
            if (shown >= 8) {
                break;
            }
            const std::uint64_t zone_tiers =
                row.lod_full + row.lod_reduced + row.lod_low + row.lod_dormant;
            const std::uint64_t zone_mobs = row.mobs;
            // Only zones that can explain the breach (a +-1 insert bump is
            // noise); retired/split parents included.
            const std::uint64_t zone_diff =
                zone_tiers > zone_mobs ? zone_tiers - zone_mobs : zone_mobs - zone_tiers;
            if (zone_diff <= 8) {
                continue;
            }
            std::printf("TIER-MISMATCH zone=%u mobs=%llu tiers=%llu tiersum=%llu "
                        "sleeping=%d sim=%d part=%u players=%llu\n",
                        row.id,
                        (unsigned long long)zone_mobs,
                        (unsigned long long)zone_tiers,
                        (unsigned long long)zone_tiers,
                        row.sleeping ? 1 : 0,
                        row.sim_enabled ? 1 : 0,
                        static_cast<unsigned>(row.partition),
                        (unsigned long long)row.players);
            ++shown;
        }
    }
    std::printf("READINESS tier_diff=%llu tolerance=%llu\n",
                (unsigned long long)tier_diff,
                (unsigned long long)tier_tolerance);
    check("tier-accounting", tier_diff <= tier_tolerance);
    std::printf("READINESS residency: players=%llu mobs=%llu ghosts=%llu active_zones=%zu "
                "sleeping_zones=%zu\n",
                (unsigned long long)resident_players,
                (unsigned long long)resident_mobs,
                (unsigned long long)ghosts,
                active_zones,
                sleeping_zones);
    const std::uint64_t tick_window_us = total_of(&ZoneStageTotals::tick_micros);
    const double stage_denominator =
        tick_window_us > 0 ? static_cast<double>(tick_window_us) : 1.0;
    std::printf("READINESS stage_ms(measure=%.1fs tick_total=%.1fms): gameplay=%.1f(%.1f%%) "
                "ai=%.1f movement=%.1f aoi=%.1f ghost=%.1f(%.1f%%) activity_publish=%.1f "
                "load_publish=%.1f repl=%.1f(%.1f%%) lod_eval=%.1f(%.1f%%) [gameplay includes "
                "ai+movement; ghost includes activity publish; replication includes AOI]\n",
                measure_actual_s,
                static_cast<double>(tick_window_us) / 1000.0,
                stage_gameplay / 1000.0,
                100.0 * static_cast<double>(stage_gameplay) / stage_denominator,
                stage_ai / 1000.0,
                stage_movement / 1000.0,
                stage_aoi / 1000.0,
                stage_ghost / 1000.0,
                100.0 * static_cast<double>(stage_ghost) / stage_denominator,
                stage_activity / 1000.0,
                stage_load_publish / 1000.0,
                stage_replication / 1000.0,
                100.0 * static_cast<double>(stage_replication) / stage_denominator,
                stage_lod_eval / 1000.0,
                100.0 * static_cast<double>(stage_lod_eval) / stage_denominator);
    (void)stage_measured_total;
    std::printf("READINESS workload: aoi_queries=%llu repl_records=%llu repl_bytes=%llu "
                "dirty=%llu aoi_candidates=%llu load_sim=%llu load_combat=%llu "
                "load_migration=%llu ghost_ops=%llu\n",
                (unsigned long long)total_aoi_queries,
                (unsigned long long)total_repl_records,
                (unsigned long long)load_window.repl_bytes,
                (unsigned long long)load_window.repl_dirty,
                (unsigned long long)load_window.aoi_candidates,
                (unsigned long long)load_window.sim_work,
                (unsigned long long)load_window.combat_events,
                (unsigned long long)load_window.migration_events,
                (unsigned long long)total_ghost_entities);
    std::printf("READINESS lod_work: ai=%llu mv=%llu prom=%llu dem=%llu wake=%llu eval_ms=%.1f\n",
                (unsigned long long)delta(counters_after.lod_ai, counters_before.lod_ai),
                (unsigned long long)delta(counters_after.lod_mv, counters_before.lod_mv),
                (unsigned long long)delta(counters_after.lod_prom, counters_before.lod_prom),
                (unsigned long long)delta(counters_after.lod_dem, counters_before.lod_dem),
                (unsigned long long)delta(counters_after.lod_wake, counters_before.lod_wake),
                static_cast<double>(delta(counters_after.lod_eval_us, counters_before.lod_eval_us)) /
                    1000.0);
    std::printf("READINESS asf: split_commits=%llu split_aborts=%llu merge_commits=%llu "
                "merge_aborts=%llu merge_eval=%llu suppressed=[eligible=%llu sustained=%llu "
                "recent_split=%llu recent_merge=%llu unsafe=%llu min_improvement=%llu "
                "split_merge_cooldown=%llu] emergency=%llu oscillation=%llu\n",
                (unsigned long long)delta(counters_after.split_commits, counters_before.split_commits),
                (unsigned long long)delta(counters_after.split_aborts, counters_before.split_aborts),
                (unsigned long long)delta(counters_after.merge_commits, counters_before.merge_commits),
                (unsigned long long)delta(counters_after.merge_aborts, counters_before.merge_aborts),
                (unsigned long long)delta(counters_after.merge_eval, counters_before.merge_eval),
                (unsigned long long)delta(counters_after.merge_sup_not_eligible,
                                          counters_before.merge_sup_not_eligible),
                (unsigned long long)delta(counters_after.merge_sup_not_sustained,
                                          counters_before.merge_sup_not_sustained),
                (unsigned long long)delta(counters_after.merge_sup_recent_split,
                                          counters_before.merge_sup_recent_split),
                (unsigned long long)delta(counters_after.merge_sup_recent_merge,
                                          counters_before.merge_sup_recent_merge),
                (unsigned long long)delta(counters_after.merge_sup_unsafe,
                                          counters_before.merge_sup_unsafe),
                (unsigned long long)delta(counters_after.merge_sup_min_improvement,
                                          counters_before.merge_sup_min_improvement),
                (unsigned long long)delta(counters_after.split_sup_merge_cooldown,
                                          counters_before.split_sup_merge_cooldown),
                (unsigned long long)delta(counters_after.split_emergency, counters_before.split_emergency),
                (unsigned long long)delta(counters_after.oscillation, counters_before.oscillation));
    std::printf("READINESS control: control_ms=%.2f observe_ms=%.2f score_ms=%.2f "
                "migration_supervisor_ms=%.2f\n",
                static_cast<double>(delta(counters_after.control_us, counters_before.control_us)) /
                    1000.0,
                static_cast<double>(delta(counters_after.observe_us, counters_before.observe_us)) /
                    1000.0,
                static_cast<double>(delta(counters_after.score_us, counters_before.score_us)) / 1000.0,
                static_cast<double>(delta(counters_after.migration_us, counters_before.migration_us)) /
                    1000.0);
    std::printf("READINESS migration: committed=%llu stale=%llu dup=%llu retry=%llu fail=%llu "
                "zone_boundary_crossings=%llu\n",
                (unsigned long long)delta(counters_after.migrations_committed,
                                          counters_before.migrations_committed),
                (unsigned long long)delta(counters_after.mig_stale, counters_before.mig_stale),
                (unsigned long long)delta(counters_after.mig_dup, counters_before.mig_dup),
                (unsigned long long)delta(counters_after.mig_retry, counters_before.mig_retry),
                (unsigned long long)delta(counters_after.mig_fail, counters_before.mig_fail),
                (unsigned long long)total_migrations);
    std::printf("READINESS routing: local=%llu remote_emulated=%llu unavailable=%llu draining=%llu\n",
                (unsigned long long)delta(counters_after.routes_local, counters_before.routes_local),
                (unsigned long long)delta(counters_after.routes_remote, counters_before.routes_remote),
                (unsigned long long)delta(counters_after.routes_unavail, counters_before.routes_unavail),
                (unsigned long long)delta(counters_after.routes_draining,
                                          counters_before.routes_draining));
    std::printf("READINESS combat: attacks=%llu deaths=%llu\n",
                (unsigned long long)delta(counters_after.attacks, counters_before.attacks),
                (unsigned long long)delta(counters_after.deaths, counters_before.deaths));

    const std::uint64_t ghost_publish_us = total_of(&ZoneStageTotals::ghost_publish_micros);
    const std::uint64_t ghost_reconcile_us = total_of(&ZoneStageTotals::ghost_reconcile_micros);
    std::printf("READINESS ghost: publish_ms=%.1f reconcile_ms=%.1f ops=%llu "
                "diff=[keep=%llu add=%llu rem=%llu] "
                "publish_diff=[upd=%llu add=%llu rem=%llu refresh=%llu skip=%llu] "
                "reconcile_skip=%llu delta=%llu full=%llu fallbacks=%llu candidates=%llu "
                "grid_ops=%llu\n",
                static_cast<double>(ghost_publish_us) / 1000.0,
                static_cast<double>(ghost_reconcile_us) / 1000.0,
                (unsigned long long)total_of(&ZoneStageTotals::ghost_entities),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_keep),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_add),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_remove),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_publish_updates),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_publish_adds),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_publish_removes),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_publish_refreshes),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_publish_skips),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_reconcile_skips),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_delta_reconciles),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_full_reconciles),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_full_fallbacks),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_candidates),
                (unsigned long long)total_of(&ZoneStageTotals::ghost_spatial_queries));
    if (config.ghost_shadow) {
        const auto ghost_stats = sim.GhostValidationSnapshot();
        std::printf("READINESS ghost_shadow: polls=%llu poll_failures=%llu validator_runs=%llu "
                    "validator_failures=%llu repairs=%llu\n",
                    (unsigned long long)ghost_shadow_runs,
                    (unsigned long long)ghost_shadow_failures,
                    (unsigned long long)ghost_stats.runs,
                    (unsigned long long)ghost_stats.failures,
                    (unsigned long long)ghost_stats.repairs);
        check("ghost-shadow-equivalence",
              ghost_shadow_failures == 0 && ghost_stats.failures == 0);
    }

    // ---- Phase 5B: AOI / replication / timing blocks (measured counters) --
    const std::uint64_t aoi_pre_cap = total_of(&ZoneStageTotals::aoi_candidates_pre_cap);
    const std::uint64_t aoi_post_cap = total_of(&ZoneStageTotals::aoi_candidates_post_cap);
    const std::uint64_t aoi_visible = total_of(&ZoneStageTotals::aoi_visible_final);
    const std::uint64_t interest_enter = total_of(&ZoneStageTotals::interest_enter);
    const std::uint64_t interest_leave = total_of(&ZoneStageTotals::interest_leave);
    const std::uint64_t interest_keep = total_of(&ZoneStageTotals::interest_keep);
    const double aoi_reduction =
        aoi_pre_cap > 0 ? 100.0 * (1.0 - static_cast<double>(aoi_post_cap) /
                                             static_cast<double>(aoi_pre_cap))
                        : 0.0;
    std::printf("READINESS aoi: queries=%llu pre_cap=%llu post_cap=%llu visible=%llu "
                "reduction=%.1f%% enter=%llu leave=%llu keep=%llu\n",
                (unsigned long long)total_of(&ZoneStageTotals::aoi_queries),
                (unsigned long long)aoi_pre_cap,
                (unsigned long long)aoi_post_cap,
                (unsigned long long)aoi_visible,
                aoi_reduction,
                (unsigned long long)interest_enter,
                (unsigned long long)interest_leave,
                (unsigned long long)interest_keep);

    const std::uint64_t repl_spawn = total_of(&ZoneStageTotals::repl_spawn);
    const std::uint64_t repl_despawn = total_of(&ZoneStageTotals::repl_despawn);
    const std::uint64_t repl_update = total_of(&ZoneStageTotals::repl_update);
    const std::uint64_t repl_suppressed = total_of(&ZoneStageTotals::repl_suppressed);
    const std::uint64_t repl_records = total_of(&ZoneStageTotals::repl_records);
    const std::uint64_t repl_fanout = total_of(&ZoneStageTotals::repl_fanout_relationships);
    const std::uint64_t repl_frame_bytes = total_of(&ZoneStageTotals::repl_frame_bytes);
    const std::uint64_t repl_payload_bytes = total_of(&ZoneStageTotals::repl_payload_bytes);
    const std::uint64_t repl_refresh = total_of(&ZoneStageTotals::repl_refresh);
    const std::uint64_t repl_potential = repl_update + repl_suppressed;
    const double suppression_ratio =
        repl_potential > 0 ? 100.0 * static_cast<double>(repl_suppressed) /
                                 static_cast<double>(repl_potential)
                           : 0.0;
    std::printf("READINESS replication: spawn=%llu despawn=%llu update=%llu suppressed=%llu "
                "suppression=%.1f%% refresh=%llu records=%llu fanout=%llu "
                "frame_bytes=%llu payload_bytes=%llu MB/s=%.2f\n",
                (unsigned long long)repl_spawn,
                (unsigned long long)repl_despawn,
                (unsigned long long)repl_update,
                (unsigned long long)repl_suppressed,
                suppression_ratio,
                (unsigned long long)repl_refresh,
                (unsigned long long)repl_records,
                (unsigned long long)repl_fanout,
                (unsigned long long)repl_frame_bytes,
                (unsigned long long)repl_payload_bytes,
                static_cast<double>(repl_frame_bytes + repl_payload_bytes) /
                    (1024.0 * 1024.0) / std::max(0.001, measure_actual_s));
    // Phase 5D AOI stage audit: measured totals plus explicitly DERIVED
    // per-query averages (the exact-distance check runs inside the traversal;
    // there is deliberately no per-candidate timer).
    {
        const std::uint64_t queries = total_of(&ZoneStageTotals::aoi_queries);
        const std::uint64_t cells = total_of(&ZoneStageTotals::aoi_cells_visited);
        const std::uint64_t entries = total_of(&ZoneStageTotals::aoi_entries_visited);
        const std::uint64_t exact = total_of(&ZoneStageTotals::aoi_exact_checks);
        const std::uint64_t post = total_of(&ZoneStageTotals::aoi_candidates_pre_cap);
        const std::uint64_t visible = total_of(&ZoneStageTotals::aoi_visible_final);
        const std::uint64_t index_us = total_of(&ZoneStageTotals::aoi_index_us);
        const std::uint64_t topk_us = total_of(&ZoneStageTotals::aoi_topk_us);
        const auto per_query = [queries](std::uint64_t value) {
            return queries > 0 ? static_cast<double>(value) / static_cast<double>(queries) : 0.0;
        };
        std::printf("READINESS aoi_stages: queries=%llu cells=%llu entries=%llu exact=%llu "
                    "post_dist=%llu visible=%llu index_us=%llu topk_us=%llu | DERIVED per_query: "
                    "cells=%.2f entries=%.2f exact=%.2f post_dist=%.2f visible=%.2f "
                    "index_us=%.2f topk_us=%.2f\n",
                    (unsigned long long)queries,
                    (unsigned long long)cells,
                    (unsigned long long)entries,
                    (unsigned long long)exact,
                    (unsigned long long)post,
                    (unsigned long long)visible,
                    (unsigned long long)index_us,
                    (unsigned long long)topk_us,
                    per_query(cells),
                    per_query(entries),
                    per_query(exact),
                    per_query(post),
                    per_query(visible),
                    per_query(index_us),
                    per_query(topk_us));
        std::printf("READINESS grid_maintenance: inserts=%llu removes=%llu move_in_cell=%llu "
                    "move_cell=%llu\n",
                    (unsigned long long)total_of(&ZoneStageTotals::grid_inserts),
                    (unsigned long long)total_of(&ZoneStageTotals::grid_removes),
                    (unsigned long long)total_of(&ZoneStageTotals::grid_moves_in_cell),
                    (unsigned long long)total_of(&ZoneStageTotals::grid_moves_cell));
    }
    const std::uint64_t repl_aoi_us = total_of(&ZoneStageTotals::repl_aoi_us);
    const std::uint64_t repl_reconcile_us = total_of(&ZoneStageTotals::repl_reconcile_us);
    const std::uint64_t repl_encode_us = total_of(&ZoneStageTotals::repl_encode_us);
    const std::uint64_t repl_send_us = total_of(&ZoneStageTotals::repl_send_us);
    std::printf("READINESS replication_timing: aoi_ms=%.1f reconcile_ms=%.1f encode_ms=%.1f "
                "send_ms=%.1f total_stage_ms=%.1f\n",
                static_cast<double>(repl_aoi_us) / 1000.0,
                static_cast<double>(repl_reconcile_us) / 1000.0,
                static_cast<double>(repl_encode_us) / 1000.0,
                static_cast<double>(repl_send_us) / 1000.0,
                static_cast<double>(stage_replication) / 1000.0);
    // Phase 5C shared-payload accounting: CPU-side reuse vs wire bytes. The
    // record requests are per-recipient needs; serializations are the unique
    // payload builds. bytes_generated = unique serialized bytes,
    // bytes_copied = application-side assembly copies, wire_bytes = bytes
    // handed to send() (the per-recipient traffic).
    const std::uint64_t record_requests = total_of(&ZoneStageTotals::repl_record_requests);
    const std::uint64_t record_serializations =
        total_of(&ZoneStageTotals::repl_record_serializations);
    const std::uint64_t despawn_hits = total_of(&ZoneStageTotals::repl_despawn_cache_hits);
    const std::uint64_t despawn_misses = total_of(&ZoneStageTotals::repl_despawn_cache_misses);
    const std::uint64_t bytes_generated = total_of(&ZoneStageTotals::repl_bytes_generated);
    const std::uint64_t bytes_copied = total_of(&ZoneStageTotals::repl_bytes_copied);
    const std::uint64_t wire_bytes = total_of(&ZoneStageTotals::repl_wire_bytes);
    const double record_reuse =
        record_requests > 0
            ? 100.0 * (1.0 - static_cast<double>(record_serializations) /
                                 static_cast<double>(record_requests))
            : 0.0;
    const double copy_amplification =
        bytes_generated > 0 ? static_cast<double>(bytes_copied) /
                                  static_cast<double>(bytes_generated)
                            : 0.0;
    std::printf("READINESS shared_payload: record_requests=%llu record_serializations=%llu "
                "record_reuse=%.1f%% despawn_cache=[hit=%llu miss=%llu] "
                "bytes=[generated=%llu copied=%llu wire=%llu copy_amplification=%.2fx]\n",
                (unsigned long long)record_requests,
                (unsigned long long)record_serializations,
                record_reuse,
                (unsigned long long)despawn_hits,
                (unsigned long long)despawn_misses,
                (unsigned long long)bytes_generated,
                (unsigned long long)bytes_copied,
                (unsigned long long)wire_bytes,
                copy_amplification);

    // Phase 6 v2: field-delta vs full-state split, deferral/budget/starvation
    // and Network LOD tier populations (MEASURED; ratios DERIVED).
    {
        const std::uint64_t v2_full = total_of(&ZoneStageTotals::repl_v2_full_records);
        const std::uint64_t v2_delta = total_of(&ZoneStageTotals::repl_v2_delta_records);
        const std::uint64_t v2_total = v2_full + v2_delta;
        const std::uint64_t v2_deferred = total_of(&ZoneStageTotals::repl_v2_deferred);
        const std::uint64_t v2_starvation = total_of(&ZoneStageTotals::repl_v2_starvation);
        const std::uint64_t v2_budget_hits = total_of(&ZoneStageTotals::repl_v2_budget_hits);
        const std::uint64_t v2_critical = total_of(&ZoneStageTotals::repl_v2_critical);
        const std::uint64_t v2_delta_bytes = total_of(&ZoneStageTotals::repl_v2_delta_bytes);
        // A per-zone peak: the world value is the max over zones, never the
        // sum (summing 160 zones' ~4-tick peaks reported a bogus ~560).
        std::uint64_t v2_max_defer = 0;
        for (const auto& [zone_id, zone] : zone_totals) {
            (void)zone_id;
            v2_max_defer = std::max(v2_max_defer, zone.repl_v2_max_defer.total);
        }
        const std::uint64_t tier_c = total_of(&ZoneStageTotals::repl_v2_tier_critical);
        const std::uint64_t tier_n = total_of(&ZoneStageTotals::repl_v2_tier_near);
        const std::uint64_t tier_no = total_of(&ZoneStageTotals::repl_v2_tier_normal);
        const std::uint64_t tier_r = total_of(&ZoneStageTotals::repl_v2_tier_reduced);
        const double delta_ratio =
            v2_total > 0 ? 100.0 * static_cast<double>(v2_delta) / static_cast<double>(v2_total)
                         : 0.0;
        std::printf("READINESS replv2: full=%llu delta=%llu delta_ratio=%.1f%% "
                    "deferred=%llu starvation=%llu budget_hits=%llu critical=%llu "
                    "delta_bytes=%llu max_defer_ticks=%llu\n",
                    (unsigned long long)v2_full,
                    (unsigned long long)v2_delta,
                    delta_ratio,
                    (unsigned long long)v2_deferred,
                    (unsigned long long)v2_starvation,
                    (unsigned long long)v2_budget_hits,
                    (unsigned long long)v2_critical,
                    (unsigned long long)v2_delta_bytes,
                    (unsigned long long)v2_max_defer);
        std::printf("READINESS netlod: critical=%llu near=%llu normal=%llu reduced=%llu\n",
                    (unsigned long long)tier_c,
                    (unsigned long long)tier_n,
                    (unsigned long long)tier_no,
                    (unsigned long long)tier_r);
    }

    // Interest-overlap measurement (phase 5C §11, report-time only): how
    // similar are the recipients' interest sets? Deterministic FNV-1a over
    // the sorted visible NetId set per viewer; exact duplicates are the
    // grouping candidates.
    {
        // Viewer sets were copied (sorted) by the report snapshot.
        // Empty interest sets are trivially "identical": measure overlap on
        // the non-empty sets only (the sparse scenarios have many idle
        // viewers and would otherwise report meaningless 90%+ duplicates).
        std::vector<std::size_t> sizes;
        sizes.reserve(report.viewer_sets.size());
        std::size_t nonempty_viewers = 0;
        std::unordered_map<std::uint64_t, std::uint32_t> nonempty;
        for (const auto& nets : report.viewer_sets) {
            sizes.push_back(nets.size());
            if (nets.empty()) {
                continue;
            }
            ++nonempty_viewers;
            std::uint64_t h = 1469598103934665603ull;
            for (const std::uint32_t net_id : nets) {
                h ^= net_id;
                h *= 1099511628211ull;
            }
            h ^= static_cast<std::uint64_t>(nets.size());
            h *= 1099511628211ull;
            ++nonempty[h];
        }
        const std::size_t nonempty_signatures = nonempty.size();
        std::uint32_t max_group = 0;
        for (const auto& [signature, count] : nonempty) {
            (void)signature;
            max_group = std::max(max_group, count);
        }
        std::sort(sizes.begin(), sizes.end());
        const std::size_t p50 =
            sizes.empty() ? 0 : sizes[static_cast<std::size_t>(
                                      static_cast<double>(sizes.size() - 1) * 0.50)];
        const std::size_t p95 =
            sizes.empty() ? 0 : sizes[static_cast<std::size_t>(
                                      static_cast<double>(sizes.size() - 1) * 0.95)];
        const std::size_t max_size = sizes.empty() ? 0 : sizes.back();
        const double avg_size =
            sizes.empty() ? 0.0
                          : static_cast<double>(std::accumulate(sizes.begin(), sizes.end(),
                                                                std::size_t{0})) /
                                static_cast<double>(sizes.size());
        std::printf("READINESS interest_overlap: viewers=%zu empty=%zu avg_size=%.1f p50=%zu "
                    "p95=%zu max=%zu nonempty_signatures=%zu max_identical_group=%u "
                    "exact_duplicate_ratio=%.1f%%\n",
                    sizes.size(),
                    sizes.size() - nonempty_viewers,
                    avg_size,
                    p50,
                    p95,
                    max_size,
                    nonempty_signatures,
                    max_group,
                    nonempty_viewers == 0
                        ? 0.0
                        : 100.0 *
                              static_cast<double>(nonempty_viewers - nonempty_signatures) /
                              static_cast<double>(nonempty_viewers));
    }

    if (config.replication_shadow) {
        const auto repl_stats = sim.ReplicationValidationSnapshot();
        std::printf("READINESS replication_shadow: polls=%llu poll_failures=%llu "
                    "validator_runs=%llu validator_failures=%llu\n",
                    (unsigned long long)replication_shadow_runs,
                    (unsigned long long)replication_shadow_failures,
                    (unsigned long long)repl_stats.runs,
                    (unsigned long long)repl_stats.failures);
        check("replication-shadow-equivalence",
              replication_shadow_failures == 0 && repl_stats.failures == 0);
    }

    const auto activity_metrics = sim.ActivityMetrics();
    const auto load_metrics = sim.LoadFieldMetrics();
    const auto activity_grid = sim.ActivitySnapshot();
    const auto load_grid = sim.LoadFieldSnapshot();
    const std::size_t activity_bytes =
        activity_grid ? activity_grid->CellCount() * sizeof(gs::game::ActivityCell) : 0;
    std::size_t source_cell_capacity=0;
    if(activity_grid)for(const auto& cell:activity_grid->cells)source_cell_capacity+=cell.players.capacity()*sizeof(gs::game::PlayerInfluenceSource);
    std::printf("TC4MEMORY phase=audit activity_cells_bytes=%zu cell_source_capacity_bytes=%zu retained_generation_input_bytes=%zu attribution=container_capacity_excludes_allocator_overhead\n",activity_bytes,source_cell_capacity,activity_grid?activity_grid->generation_sources.capacity()*sizeof(gs::game::PlayerInfluenceSource):0);
    const std::size_t load_bytes =
        load_grid ? load_grid->CellCount() * sizeof(gs::game::LoadCell) +
                        load_grid->L1CellCount() * sizeof(gs::game::LoadCell)
                  : 0;
    std::printf("READINESS fields: activity=[sources=%llu cells=%llu rebuilds=%llu rebuild_ms=%.1f] "
                "load=[cells=%llu active=%llu rebuilds=%llu rebuild_ms=%.1f]\n",
                (unsigned long long)activity_metrics.sources,
                (unsigned long long)activity_metrics.cells_nonempty,
                (unsigned long long)activity_metrics.rebuilds,
                static_cast<double>(activity_metrics.rebuild_us_total) / 1000.0,
                (unsigned long long)load_metrics.cells_total,
                (unsigned long long)load_metrics.cells_active,
                (unsigned long long)load_metrics.rebuilds,
                static_cast<double>(load_metrics.rebuild_us_total) / 1000.0);
    // Working set, partition nodes and zone slots: same snapshot epoch.
    std::printf("READINESS memory: working_set_mb=%.1f activity_grid_mb=%.2f load_grid_mb=%.2f "
                "partition_nodes=%zu zones=%zu epoch=%llu\n",
                static_cast<double>(report.working_set_bytes) / (1024.0 * 1024.0),
                static_cast<double>(activity_bytes) / (1024.0 * 1024.0),
                static_cast<double>(load_bytes) / (1024.0 * 1024.0),
                report.partition_nodes,
                report.zone_slots,
                (unsigned long long)report.epoch);
    std::printf("READINESS validation: %s\n", validated ? "OK" : validation_result.c_str());
    if (config.file_world) {
        const auto& ts = terrain_after_measure;
        const auto& s = ts.streamer;
        // Window observations were frozen before the final audit.
        std::printf("READINESS terrain: measure_elapsed_s=%.2f loads=%llu evictions=%llu misses=%llu "
                    "wait_steps=%llu invalid=%llu peak_accounted_mb=%.2f budget_mb=%.2f "
                    "resident=%zu queued_peak=%zu recent_miss_p99_ms=%.2f bytes_read=%llu\n",
                    measure_actual_s,
                    (unsigned long long)(s.loads_completed - terrain_start.streamer.loads_completed),
                    (unsigned long long)(s.evictions - terrain_start.streamer.evictions),
                    (unsigned long long)(s.misses - terrain_start.streamer.misses),
                    (unsigned long long)(ts.queries.steps_waiting - terrain_start.queries.steps_waiting),
                    (unsigned long long)(ts.queries.invalid - terrain_start.queries.invalid),
                    s.peak_accounted_bytes / 1048576.0, s.budget_bytes / 1048576.0,
                    s.resident, s.waiting_high_water, s.miss_ms_p99,
                    (unsigned long long)(s.bytes_read - terrain_start.streamer.bytes_read));
        std::printf("READINESS terrain-ledger: accounted=%zu resident=%zu inflight=%zu retired=%zu metadata=%zu "
                    "pinned=%zu peak_accounted=%zu peak_inflight=%zu peak_retired=%zu peak_pinned_sampled=%zu budget=%zu\n",
                    s.accounted_bytes,s.resident_bytes,s.in_flight_bytes,s.retired_bytes,s.metadata_bytes,
                    s.pinned_bytes,s.peak_accounted_bytes,s.peak_in_flight_bytes,s.peak_retired_bytes,
                    s.peak_pinned_bytes,s.budget_bytes);
        std::printf("READINESS terrain-work: measure_pumps=%llu work_us=%llu room_checks=%llu blocked=%llu "
                    "retry_suppressed=%llu frees=%llu safepoints=%llu drain_wait_us=%llu (elapsed work, not OS CPU samples)\n",
                    (unsigned long long)(s.pump_calls-terrain_start.streamer.pump_calls),
                    (unsigned long long)(s.pump_us-terrain_start.streamer.pump_us),
                    (unsigned long long)(s.room_checks-terrain_start.streamer.room_checks),
                    (unsigned long long)(s.room_blocked-terrain_start.streamer.room_blocked),
                    (unsigned long long)(s.retry_suppressed-terrain_start.streamer.retry_suppressed),
                    (unsigned long long)(s.frees-terrain_start.streamer.frees),
                    (unsigned long long)(ts.safepoints-terrain_start.safepoints),
                    (unsigned long long)(ts.drain_wait_us-terrain_start.drain_wait_us));
        if (!config.eager_terrain) {
            check("terrain-budget", s.peak_accounted_bytes <= s.budget_bytes);
            if((scenario=="c-moving-v1" || scenario=="c-moving-v2")) {
                check("c-moving-measure-read-evict-reload",s.reloads>0 && s.bytes_read>terrain_start.streamer.bytes_read && s.evictions>terrain_start.streamer.evictions);
                check("c-moving-three-relocations",relocation_step>=3);
                check("c-moving-terrain-height-oracle",requested_positions_present());
            }
            if (scenario == "moving") check("real-streaming-churn",
                s.loads_completed > terrain_start.streamer.loads_completed &&
                s.evictions > terrain_start.streamer.evictions);
        }
    }
    std::printf("READINESS-DONE failures=%d\n", failures);
    sim.Stop();
    return failures;
}

} // namespace gs::bench
