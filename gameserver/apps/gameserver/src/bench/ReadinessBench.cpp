#include "ReadinessBench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "../world/WorldRuntime.h"
#include "../world/partition/PartitionScoring.h"
#include "../world/spawn/SpawnLoader.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // PROCESS_MEMORY_COUNTERS (K32GetProcessMemoryInfo is in kernel32)
#endif

namespace gs::bench {

namespace {

using Clock = std::chrono::steady_clock;

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
// faster than that reconstructs the totals (a value that dropped means the
// window was exchanged and the previous value is the full window).
struct WindowDelta {
    std::uint64_t last = 0;
    std::uint64_t total = 0;
};

void AccumulateWindow(std::uint64_t current, WindowDelta& delta)
{
    if (current >= delta.last) {
        delta.total += current - delta.last;
    } else {
        delta.total += delta.last;
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
    gs::game::WorldRuntime sim(io, {},
                               gs::game::WorldRuntime::SyntheticWorldConfig{
                                   extent, static_cast<std::uint32_t>(zones_x),
                                   static_cast<std::uint32_t>(zones_y), {}});
    check("zones-built", sim.Zones().ZoneCount() == expected_zones);

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
    if (config.ghost_shadow) {
        // Correctness run: a validator-detected inconsistency must stay
        // visible (no auto-repair) so the equivalence proof is honest.
        sim.SetGhostAutoRepair(false);
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
            const float x = boundary_x - 50.0f - static_cast<float>(i % 5) * 8.0f;
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

    std::vector<BenchPlayer> players(static_cast<std::size_t>(config.players));
    const auto player_spawn_start = Clock::now();
    for (int i = 0; i < config.players; ++i) {
        auto& player = players[static_cast<std::size_t>(i)];
        player.session_id = static_cast<gs::common::SessionId>(100 + i);
        player.character_index = i;
        player.x = player_positions[static_cast<std::size_t>(i)].first;
        player.y = player_positions[static_cast<std::size_t>(i)].second;
        boost::asio::ip::tcp::socket socket(io);
        player.session = std::make_shared<gs::network::Session>(std::move(socket),
                                                                player.session_id);
        sim.PostSpawn(player.session,
                      MakeReadinessCharacter(player.character_index),
                      gs::game::DebugSpawnOverride{player.x, player.y});
    }
    const bool players_populated = WaitFor(std::chrono::seconds(180), [&] {
        return sim.Owners().size() == static_cast<std::size_t>(config.players);
    });
    const double player_spawn_s =
        std::chrono::duration<double>(Clock::now() - player_spawn_start).count();
    std::printf("READINESS setup: players_spawned=%zu player_spawn_s=%.2f\n",
                sim.Owners().size(),
                player_spawn_s);
    check("players-spawned", players_populated);
    if (!players_populated) {
        sim.Stop();
        return failures + 1;
    }

    // ---- WARMUP -----------------------------------------------------------
    std::printf("READINESS warmup: %ds (fields, LOD, partition state settle)\n",
                config.warmup_seconds);
    const auto warmup_end = Clock::now() + std::chrono::seconds(config.warmup_seconds);
    while (Clock::now() < warmup_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ---- MEASURE ----------------------------------------------------------
    std::vector<ZoneStageTotals> zone_totals(sim.Zones().ZoneCount());
    auto sample_zones = [&]() {
        // The partition controller may split zones during the measurement
        // (zone slots are append-only), so the accumulator array grows with
        // the live zone count.
        if (zone_totals.size() < sim.Zones().ZoneCount()) {
            zone_totals.resize(sim.Zones().ZoneCount());
        }
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
            auto& totals = zone_totals[zi];
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
        }
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
        } else if (scenario == "moving" || scenario == "churn") {
            // Relocate the player hotspot (production despawn + spawn). The
            // mob population stays; player influence / AOI / replication move.
            const float path[3][2] = {
                {extent * 0.2f, extent * 0.3f}, {extent * 0.5f, extent * 0.5f},
                {extent * 0.8f, extent * 0.7f}};
            const int waypoint = relocation_step % 3;
            for (auto& player : players) {
                sim.PostDespawn(player.session_id);
            }
            WaitFor(std::chrono::seconds(20), [&] { return sim.Owners().empty(); });
            for (auto& player : players) {
                const float t = static_cast<float>(player.character_index) * 2.39996323f;
                const float r = 400.0f * std::sqrt(
                    (static_cast<float>(player.character_index) + 0.5f) /
                    static_cast<float>(std::max(1, config.players)));
                player.x = path[waypoint][0] + std::cos(t) * r;
                player.y = path[waypoint][1] + std::sin(t) * r;
                boost::asio::ip::tcp::socket socket(io);
                player.session = std::make_shared<gs::network::Session>(std::move(socket),
                                                                        player.session_id);
                sim.PostSpawn(player.session,
                              MakeReadinessCharacter(player.character_index),
                              gs::game::DebugSpawnOverride{player.x, player.y});
            }
            ++relocation_step;
            next_action = now + std::chrono::seconds(
                                    scenario == "churn" ? 12 : config.measure_seconds / 3);
        } else {
            next_action = now + std::chrono::seconds(2);
        }
    };

    std::printf("READINESS measure: %ds ghost_shadow=%d\n",
                config.measure_seconds,
                config.ghost_shadow ? 1 : 0);
    const auto measure_start = Clock::now();
    const auto measure_end = measure_start + std::chrono::seconds(config.measure_seconds);
    auto next_sample = measure_start;
    auto next_ghost_shadow = measure_start;
    std::uint64_t ghost_shadow_runs = 0;
    std::uint64_t ghost_shadow_failures = 0;
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
        run_actions(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sample_zones();
    sample_load_field();
    const GlobalCounters counters_after = snapshot_globals();
    const double measure_actual_s =
        std::chrono::duration<double>(Clock::now() - measure_start).count();

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
    std::size_t stuck_zones = 0;
    std::uint64_t zone_ticks = 0;
    for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
        auto& zone = sim.Zones().GetZone(zi);
        if (zone.TickInProgress().load(std::memory_order_acquire)) {
            ++stuck_zones;
        }
        zone_ticks += zone.TickIndex();
    }
    std::printf("READINESS validation_wait_s=%.1f stuck_zones=%zu zone_ticks=%llu\n",
                validation_wait_s,
                stuck_zones,
                (unsigned long long)zone_ticks);
    check("validation", validated);

    // ---- REPORT -----------------------------------------------------------
    std::vector<std::uint64_t> tick_samples;
    tick_samples.reserve(sim.Zones().ZoneCount() * 64);
    std::vector<std::uint64_t> zone_scratch(256, 0u);
    std::size_t worst_zone = 0;
    double worst_zone_p99 = 0.0;
    for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
        const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
        const std::size_t count = diag.CopyTickSamples(zone_scratch.data(), zone_scratch.size());
        std::vector<std::uint64_t> local(zone_scratch.begin(), zone_scratch.begin() + count);
        if (!local.empty()) {
            const double local_p99 = PercentileMs(local, 0.99);
            if (local_p99 > worst_zone_p99) {
                worst_zone_p99 = local_p99;
                worst_zone = zi;
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
    for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
        const auto& zone = sim.Zones().GetZone(zi);
        const auto& diag = zone.Diagnostics();
        lod_full += diag.lod_full.load(std::memory_order_relaxed);
        lod_reduced += diag.lod_reduced.load(std::memory_order_relaxed);
        lod_low += diag.lod_low.load(std::memory_order_relaxed);
        lod_dormant += diag.lod_dormant.load(std::memory_order_relaxed);
        resident_players += diag.player_count.load(std::memory_order_relaxed);
        resident_mobs += diag.mob_count.load(std::memory_order_relaxed);
        ghosts += diag.ghost_count.load(std::memory_order_relaxed);
        if (zone.Diagnostics().player_count.load(std::memory_order_relaxed) > 0 ||
            zone.Diagnostics().mob_count.load(std::memory_order_relaxed) > 0) {
            ++active_zones;
        }
        if (zone.Activity() == gs::game::ZoneActivity::Sleeping) {
            ++sleeping_zones;
        }
    }

    auto total_of = [&](WindowDelta ZoneStageTotals::*member) {
        std::uint64_t total = 0;
        for (const auto& zone : zone_totals) {
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
                sim.Zones().ZoneCount(),
                tick_samples.size(),
                tick_avg,
                tick_p50,
                tick_p95,
                tick_p99,
                tick_max);
    if (worst_zone < sim.Zones().ZoneCount()) {
        std::printf("READINESS tick worst-zone: zone_id=%u p99_ms=%.3f\n",
                    sim.Zones().GetZone(worst_zone).Id(),
                    worst_zone_p99);
    }
    std::printf("READINESS tiers: full=%llu reduced=%llu low=%llu dormant=%llu (mobs=%llu)\n",
                (unsigned long long)lod_full,
                (unsigned long long)lod_reduced,
                (unsigned long long)lod_low,
                (unsigned long long)lod_dormant,
                (unsigned long long)resident_mobs);
    // Tolerance: the report reads live counters while the world keeps
    // ticking; tier gauges are eventual-consistent (1 Hz recount + insert
    // bumps during transfers), so a live sum can differ transiently. A
    // systematic leak (stale retired/tombstoned gauges) would exceed this.
    const std::uint64_t tier_sum = lod_full + lod_reduced + lod_low + lod_dormant;
    const std::uint64_t tier_diff =
        tier_sum > resident_mobs ? tier_sum - resident_mobs : resident_mobs - tier_sum;
    const std::uint64_t tier_tolerance = std::max<std::uint64_t>(32, resident_mobs / 500);
    if (tier_diff > tier_tolerance) {
        std::size_t shown = 0;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount() && shown < 8; ++zi) {
            const auto& zone = sim.Zones().GetZone(zi);
            const auto& diag = zone.Diagnostics();
            const std::uint64_t zone_tiers = diag.lod_full.load(std::memory_order_relaxed) +
                                             diag.lod_reduced.load(std::memory_order_relaxed) +
                                             diag.lod_low.load(std::memory_order_relaxed) +
                                             diag.lod_dormant.load(std::memory_order_relaxed);
            const std::uint64_t zone_mobs = diag.mob_count.load(std::memory_order_relaxed);
            if (zone_tiers == zone_mobs) {
                continue;
            }
            std::printf("TIER-MISMATCH zone=%u mobs=%llu tiers=%llu tiersum=%llu "
                        "sleeping=%d sim=%d part=%u players=%llu\n",
                        zone.Id(),
                        (unsigned long long)zone_mobs,
                        (unsigned long long)zone_tiers,
                        (unsigned long long)zone_tiers,
                        zone.Activity() == gs::game::ZoneActivity::Sleeping ? 1 : 0,
                        zone.SimulationEnabled() ? 1 : 0,
                        static_cast<unsigned>(zone.Partition()),
                        (unsigned long long)diag.player_count.load(std::memory_order_relaxed));
            ++shown;
        }
    }
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

    const auto activity_metrics = sim.ActivityMetrics();
    const auto load_metrics = sim.LoadFieldMetrics();
    const auto activity_grid = sim.ActivitySnapshot();
    const auto load_grid = sim.LoadFieldSnapshot();
    const std::size_t activity_bytes =
        activity_grid ? activity_grid->CellCount() * sizeof(gs::game::ActivityCell) : 0;
    const std::size_t load_bytes =
        load_grid ? load_grid->CellCount() * sizeof(gs::game::LoadCell) +
                        load_grid->L1CellCount() * sizeof(gs::game::LoadCell)
                  : 0;
    const std::size_t partition_nodes = CountPartitionNodes(sim.Zones().PartitionRoots());
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
    std::printf("READINESS memory: working_set_mb=%.1f activity_grid_mb=%.2f load_grid_mb=%.2f "
                "partition_nodes=%zu zones=%zu\n",
                static_cast<double>(ProcessWorkingSetBytes()) / (1024.0 * 1024.0),
                static_cast<double>(activity_bytes) / (1024.0 * 1024.0),
                static_cast<double>(load_bytes) / (1024.0 * 1024.0),
                partition_nodes,
                sim.Zones().ZoneCount());
    std::printf("READINESS validation: %s\n", validated ? "OK" : validation_result.c_str());
    std::printf("READINESS-DONE failures=%d\n", failures);
    sim.Stop();
    return failures;
}

} // namespace gs::bench
