// World load/benchmark harness. Separate target behind ENABLE_WORLDBENCH;
// never linked into production. Drives WorldRuntime through its public
// Post* API only (like real clients), with synthetic sessions and load
// scenarios:
//
//   A) 100 players / 1,000 mobs            (spread)
//   B) 500 players / 5,000 mobs            (spread)
//   C) 1,000 players / 10,000 mobs         (spread)
//   D) 4,000 players / 50,000 mobs         (spread, heavy)
//   E) 1,000 players concentrated          (hotspot: AOI/replication stress)
//   F) border migration stress             (many entities crossing zones)
//   G) dense AOI stress                    (many entities in a small area)
//
// Usage:
//   worldbench [--players N] [--mobs M] [--seconds S]
//              [--mode spread|hotspot|border|dense|splitmerge|lod|activity]
//              [--validate-every K] [--despawn-storm R] [--seed S]
//              [--logical-processes K] [--routing-selftest]
// Logical distribution (§33/§51): --logical-processes stripes zones across
// K logical processes in this binary; routing/migration treat them as
// remote-emulated while all delivery stays local.
//
// Measures zone-tick p50/p95/p99/max (per-zone ring buffers), worker
// utilization, supervisor time, migrations, deaths/respawns, and runs the
// consistency validator periodically when requested.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "network/Session.h"

#include "../world/activity/ContinuousLoadField.h"
#include "../world/activity/LoadFieldPublisher.h"
#include "../world/migration/EntityTransfer.h"
#include "../world/partition/PartitionScoring.h"
#include "../world/partition/ZonePartition.h"
#include "../world/WorldRuntime.h"
#include "../world/spawn/SpawnLoader.h"
#include "ReadinessBench.h"

#include <flecs.h>

namespace {

struct BenchConfig {
    int players = 100;
    int mobs = 1000;
    int seconds = 30;
    std::string mode = "spread";
    int validate_every = 0;
    int despawn_storm = 0;
    int logical_processes = 0;
    bool routing_selftest = false;
    // Pure ActivityGrid unit checks (no world, no threads): origin-aware
    // indexing and the Fast/Exact query split. Runs standalone and exits.
    bool field_selftest = false;
    // Pure ContinuousLoadField unit checks (no world, no threads): mapping,
    // zone bins, normalization, smoothing, L1, config validation.
    bool load_field_selftest = false;
    // Pure Adaptive Partition Scoring checks (no world, no threads):
    // aggregation, hotspot detection, candidates, balance/boundary formulas,
    // instability, gate semantics, config validation.
    bool partition_score_selftest = false;
    std::uint32_t seed = 12345;
    // splitmerge scenario: deterministic transfer-failure injection counts
    // (0 = commit path; >0 = abort path expectations). fail_after lets that
    // many transfers succeed first (mid-batch abort, non-empty rollback).
    int fail_snapshot = 0;
    int fail_apply = 0;
    int fail_after = 0;
    // Simulation LOD master switch for A/B runs (default on).
    bool lod_off = false;
    // Continuous load field master switch for instrumentation-overhead A/B
    // runs (default on; --loadfield-off restores the pre-instrumentation path).
    bool load_field_off = false;
    // Optional partition floor override in meters (0 = production default).
    // Lets load-driven scenarios split small test maps; still passes through
    // ValidatePartitionConfig (AOI floor clamp applies).
    int partition_min_size = 0;
    // Phase-4 integrated readiness benchmark (synthetic world).
    std::string scenario = "spread";
    float world_km = 100.0f;
    int zones_x = 8;
    int zones_y = 8;
    int warmup_seconds = 15;
    bool asf_off = false;
    bool ghost_shadow = false;
    bool replication_shadow = false;
    bool repl_full = false;
    bool aoi_full_sort = false;
};

bool ParseArgs(int argc, char** argv, BenchConfig& config)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name, std::string& out) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--help" || arg == "-h") {
            std::cout << "worldbench [--players N] [--mobs M] [--seconds S]\n"
                         "             [--mode spread|hotspot|border|dense|splitmerge|lod|activity|loadfield|partitionscore|stability|ghost|readiness]\n"
                         "             [--validate-every K] [--despawn-storm R] [--seed S]\n"
                         "             [--logical-processes K] [--routing-selftest]\n"
                         "             [--field-selftest] [--loadfield-selftest] [--partitionscore-selftest]\n"
                         "             [--fail-snapshot N] [--fail-apply N] [--fail-after N]\n"
                         "             [--partition-min-size M] [--lod-off] [--loadfield-off]\n"
                         "             [--scenario NAME] [--world-km K] [--zones-x N] [--zones-y N]\n"
                         "             [--warmup S] [--asf-off]\n";
            return false;
        } else if (arg == "--players") {
            if (!need_value("players", value)) {
                return false;
            }
            config.players = std::stoi(value);
        } else if (arg == "--mobs") {
            if (!need_value("mobs", value)) {
                return false;
            }
            config.mobs = std::stoi(value);
        } else if (arg == "--seconds") {
            if (!need_value("seconds", value)) {
                return false;
            }
            config.seconds = std::stoi(value);
        } else if (arg == "--mode") {
            if (!need_value("mode", value)) {
                return false;
            }
            config.mode = value;
        } else if (arg == "--validate-every") {
            if (!need_value("validate-every", value)) {
                return false;
            }
            config.validate_every = std::stoi(value);
        } else if (arg == "--despawn-storm") {
            if (!need_value("despawn-storm", value)) {
                return false;
            }
            config.despawn_storm = std::stoi(value);
        } else if (arg == "--seed") {
            if (!need_value("seed", value)) {
                return false;
            }
            config.seed = static_cast<std::uint32_t>(std::stoul(value));
        } else if (arg == "--logical-processes") {
            if (!need_value("logical-processes", value)) {
                return false;
            }
            config.logical_processes = std::stoi(value);
        } else if (arg == "--routing-selftest") {
            config.routing_selftest = true;
        } else if (arg == "--field-selftest") {
            config.field_selftest = true;
        } else if (arg == "--loadfield-selftest") {
            config.load_field_selftest = true;
        } else if (arg == "--partitionscore-selftest") {
            config.partition_score_selftest = true;
        } else if (arg == "--fail-snapshot") {
            if (!need_value("fail-snapshot", value)) {
                return false;
            }
            config.fail_snapshot = std::stoi(value);
        } else if (arg == "--fail-apply") {
            if (!need_value("fail-apply", value)) {
                return false;
            }
            config.fail_apply = std::stoi(value);
        } else if (arg == "--fail-after") {
            if (!need_value("fail-after", value)) {
                return false;
            }
            config.fail_after = std::stoi(value);
        } else if (arg == "--partition-min-size") {
            if (!need_value("partition-min-size", value)) {
                return false;
            }
            config.partition_min_size = std::stoi(value);
        } else if (arg == "--lod-off") {
            config.lod_off = true;
        } else if (arg == "--loadfield-off") {
            config.load_field_off = true;
        } else if (arg == "--scenario") {
            if (!need_value("scenario", value)) {
                return false;
            }
            config.scenario = value;
        } else if (arg == "--world-km") {
            if (!need_value("world-km", value)) {
                return false;
            }
            config.world_km = std::stof(value);
        } else if (arg == "--zones-x") {
            if (!need_value("zones-x", value)) {
                return false;
            }
            config.zones_x = std::stoi(value);
        } else if (arg == "--zones-y") {
            if (!need_value("zones-y", value)) {
                return false;
            }
            config.zones_y = std::stoi(value);
        } else if (arg == "--warmup") {
            if (!need_value("warmup", value)) {
                return false;
            }
            config.warmup_seconds = std::stoi(value);
        } else if (arg == "--asf-off") {
            config.asf_off = true;
        } else if (arg == "--ghost-shadow") {
            config.ghost_shadow = true;
        } else if (arg == "--replication-shadow") {
            config.replication_shadow = true;
        } else if (arg == "--repl-full") {
            config.repl_full = true;
        } else if (arg == "--aoi-full-sort") {
            config.aoi_full_sort = true;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            return false;
        }
    }
    if (config.mode != "spread" && config.mode != "hotspot" && config.mode != "border" &&
        config.mode != "dense" && config.mode != "splitmerge" && config.mode != "lod" &&
        config.mode != "activity" && config.mode != "loadfield" &&
        config.mode != "partitionscore" && config.mode != "stability" &&
        config.mode != "readiness" && config.mode != "ghost" && config.mode != "aoi" &&
        config.mode != "replication") {
        std::cerr << "bad mode: " << config.mode << "\n";
        return false;
    }
    return true;
}

double Percentile(std::vector<std::uint64_t> samples, double p)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index =
        std::min(samples.size() - 1,
                 static_cast<std::size_t>(std::floor(p / 100.0 * samples.size())));
    return static_cast<double>(samples[index]) / 1000.0;
}

gs::db::Character MakeBenchCharacter(int index)
{
    gs::db::Character character;
    character.id = gs::db::CharacterId{static_cast<std::uint64_t>(1000 + index)};
    character.account_id = gs::db::AccountId{static_cast<std::uint64_t>(5000 + index)};
    character.slot = 0;
    character.name = "Bench" + std::to_string(index);
    character.level = 1;
    character.experience = 0;
    character.class_id = 1;
    character.appearance = 0;
    character.pos_x = 0;
    character.pos_y = 0;
    character.map_id = 0;
    character.created_at = std::chrono::system_clock::now();
    character.last_played_at = character.created_at;
    return character;
}

struct BenchClient {
    std::shared_ptr<gs::network::Session> session;
    gs::common::SessionId session_id = 0;
    bool alive = false;
    float heading = 0.0f;
    std::uint32_t seq = 0;
    int slot = 0;
    int target_cursor = 0;
};

void SelftestReport(const char* name, bool pass, bool skipped, int& failures)
{
    if (skipped) {
        std::printf("SELFTEST %s: SKIP\n", name);
    } else if (pass) {
        std::printf("SELFTEST %s: PASS\n", name);
    } else {
        std::printf("SELFTEST %s: FAIL\n", name);
        ++failures;
    }
}

// --- pure ActivityGrid checks (no world, no threads, no timing) -------------
// They pin the two generalizations of the activity field:
//   * origin-aware indexing: a world need not start at (0,0) nor be square
//   * the Fast/Exact query split: identical tier, differing provenance
gs::game::ActivityGrid MakeTestGrid(gs::game::WorldBounds bounds,
                                    float cell,
                                    const std::vector<gs::game::PlayerInfluenceSource>& sources)
{
    gs::game::ActivityGrid grid;
    grid.enabled = true;
    grid.radii = gs::game::ActivityRadii{}; // 150 / 500 / 1500 m
    grid.cell_size_m = cell;
    grid.bounds = bounds;
    grid.dim_x = static_cast<std::uint32_t>(std::ceil(bounds.ExtentX() / cell));
    grid.dim_y = static_cast<std::uint32_t>(std::ceil(bounds.ExtentY() / cell));
    grid.cells.resize(static_cast<std::size_t>(grid.dim_x) * grid.dim_y);
    for (const auto& source : sources) {
        const std::size_t index =
            static_cast<std::size_t>(grid.ClampedCellY(source.y)) * grid.dim_x +
            grid.ClampedCellX(source.x);
        grid.cells[index].players.push_back(source);
    }
    return grid;
}

int RunFieldSelftest()
{
    using gs::game::ActivityGrid;
    using gs::game::PlayerInfluenceSource;
    using gs::game::SimulationTier;
    using gs::game::WorldBounds;

    int failures = 0;

    // (1) Negative origin: a source deep in the negative quadrant keeps its own
    // cell instead of being folded into cell 0, and is found from nearby.
    {
        const WorldBounds bounds{-50000.0f, -50000.0f, 50000.0f, 50000.0f};
        const ActivityGrid grid =
            MakeTestGrid(bounds, 500.0f, {PlayerInfluenceSource{1, -40000.0f, -40000.0f, 7, 0}});
        const bool dims_ok = grid.dim_x == 200u && grid.dim_y == 200u;
        const bool index_ok = grid.ClampedCellX(-50000.0f) == 0u &&
                              grid.ClampedCellX(-40000.0f) == 20u &&
                              grid.ClampedCellY(-40000.0f) == 20u &&
                              grid.ClampedCellX(0.0f) == 100u;
        const auto near_hit = grid.QueryPlayerInfluenceExact(-39900.0f, -40000.0f, 7);
        const auto far_miss = grid.QueryPlayerInfluenceExact(40000.0f, 40000.0f, 7);
        SelftestReport("field-negative-origin",
                       dims_ok && index_ok && near_hit.tier == SimulationTier::Full &&
                           !far_miss.has_influence,
                       false,
                       failures);
    }

    // (2) Non-square world: the per-axis dims are independent.
    {
        const WorldBounds bounds{0.0f, 0.0f, 4000.0f, 1000.0f};
        const ActivityGrid grid =
            MakeTestGrid(bounds, 500.0f, {PlayerInfluenceSource{1, 3900.0f, 900.0f, 3, 0}});
        const auto hit = grid.QueryPlayerInfluenceExact(3900.0f, 900.0f, 3);
        SelftestReport("field-non-square-bounds",
                       grid.dim_x == 8u && grid.dim_y == 2u &&
                           hit.tier == SimulationTier::Full && hit.nearest_sq == 0.0f,
                       false,
                       failures);
    }

    // (3) Fast vs Exact, crafted so the early-out actually matters: two sources
    // inside the Full bubble on the SAME cell row, the NEARER one in the later
    // cell. Fast stops at the farther one (its tier is already maxed); Exact
    // keeps walking and finds the true minimum.
    {
        const ActivityGrid grid = MakeTestGrid(
            WorldBounds::FromExtent(2000.0f),
            500.0f,
            {PlayerInfluenceSource{1, 960.0f, 1200.0f, 11, 0},     // cell x=1, d=40
             PlayerInfluenceSource{2, 1010.0f, 1200.0f, 22, 0}});  // cell x=2, d=10
        const auto fast = grid.QueryPlayerTierFast(1000.0f, 1200.0f, 22);
        const auto exact = grid.QueryPlayerInfluenceExact(1000.0f, 1200.0f, 22);
        // Contract: tier + has_influence are identical in both variants.
        const bool agree = fast.tier == exact.tier && fast.tier == SimulationTier::Full &&
                           fast.has_influence && exact.has_influence;
        // Exact is the true minimum (10m -> 100), Fast settled for 40m -> 1600.
        const bool exact_is_minimum =
            exact.nearest_sq == 100.0f && fast.nearest_sq == 1600.0f;
        // Documents the caveat: Fast provenance is best-effort. The fast walk
        // attributes the sample to zone 11, the exact walk to the real zone 22.
        const bool provenance_differs = fast.cross_zone && !exact.cross_zone;
        SelftestReport("field-fast-vs-exact",
                       agree && exact_is_minimum && provenance_differs,
                       false,
                       failures);
    }

    // (4) Invariant sweep: across many placements Fast and Exact must ALWAYS
    // agree on tier and has_influence, and Exact never reports a larger
    // distance than Fast. Deterministic pattern, no RNG.
    {
        std::vector<PlayerInfluenceSource> sources;
        for (int i = 0; i < 40; ++i) {
            sources.push_back(
                PlayerInfluenceSource{static_cast<std::uint32_t>(i + 1),
                                      60.0f + static_cast<float>(i) * 97.0f,
                                      40.0f + static_cast<float>((i * 53) % 1900),
                                      static_cast<std::uint32_t>(i % 3),
                                      0});
        }
        const ActivityGrid grid = MakeTestGrid(WorldBounds::FromExtent(2000.0f), 500.0f, sources);
        bool ok = true;
        int checked = 0;
        for (int gy = 0; gy < 40 && ok; ++gy) {
            for (int gx = 0; gx < 40 && ok; ++gx) {
                const auto fast =
                    grid.QueryPlayerTierFast(static_cast<float>(gx) * 50.0f,
                                             static_cast<float>(gy) * 50.0f, 0);
                const auto exact =
                    grid.QueryPlayerInfluenceExact(static_cast<float>(gx) * 50.0f,
                                                   static_cast<float>(gy) * 50.0f, 0);
                ++checked;
                if (fast.tier != exact.tier || fast.has_influence != exact.has_influence ||
                    exact.nearest_sq > fast.nearest_sq) {
                    ok = false;
                }
            }
        }
        std::printf("FIELD sweep points=%d\n", checked);
        SelftestReport("field-fast-exact-sweep", ok, false, failures);
    }

    std::printf("FIELD-SELFTEST-DONE failures=%d\n", failures);
    return failures;
}

// --- pure ContinuousLoadField checks (no world, no threads, no timing) ------
// They pin the load field's contracts: origin-aware mapping, zone-bin sparse
// publish + reset, normalization (budgets, cadence independence, clamping,
// NaN/Inf), asymmetric EMA rise/fall/spike/decay, exact L1 block sums, config
// validation and composite weighting.
int RunLoadFieldSelftest()
{
    using namespace gs::game;
    int failures = 0;

    // (1) Mapping: negative origin, non-square extents, out-of-bounds clamp.
    {
        const auto square =
            LoadFieldMapping::FromBounds({-50000.0f, -50000.0f, 50000.0f, 50000.0f}, 500.0f);
        const bool dims = square.dim_x == 200 && square.dim_y == 200;
        const bool idx = square.CellX(-50000.0f) == 0 && square.CellX(-40000.0f) == 20 &&
                         square.CellY(-40000.0f) == 20 && square.CellX(0.0f) == 100 &&
                         square.CellX(40000.0f) == 180 && square.CellX(60000.0f) == 199;
        const auto narrow = LoadFieldMapping::FromBounds({0.0f, 0.0f, 4000.0f, 1000.0f}, 500.0f);
        const bool non_square = narrow.dim_x == 8 && narrow.dim_y == 2;
        SelftestReport("loadfield-mapping", dims && idx && non_square, false, failures);
    }

    // (2) Zone bins: local rectangle, one index per touched cell, sparse
    // publish, post-publish reset, boundary margin, far-miss.
    {
        const auto mapping = LoadFieldMapping::FromBounds({0.0f, 0.0f, 1000.0f, 1000.0f}, 100.0f);
        ZoneLoadBins bins;
        bins.Configure(mapping, mx::map::Rect{0.0f, 0.0f, 500.0f, 500.0f});
        bool ok = bins.Enabled();
        auto* cell = bins.CellFor(250.0f, 250.0f);
        auto* same = bins.CellFor(260.0f, 260.0f);
        if (cell == nullptr || same == nullptr || cell != same) {
            ok = false;
        } else {
            cell->sim_work = 3;
            cell->repl_bytes = 100;
            same->sim_work += 1; // same cell: deduped in the touched list
        }
        std::vector<LoadBinEntry> out;
        bins.MovePendingTo(out);
        const bool sparse = out.size() == 1 && out[0].gx == 2 && out[0].gy == 2 &&
                            out[0].counters.sim_work == 4 && out[0].counters.repl_bytes == 100;
        std::vector<LoadBinEntry> again;
        bins.MovePendingTo(again);
        const bool reset = again.empty();
        const bool margin_hit = bins.CellFor(-10.0f, 250.0f) != nullptr;
        const bool far_miss = bins.CellFor(900.0f, 900.0f) == nullptr;
        SelftestReport("loadfield-zone-bins", ok && sparse && reset && margin_hit && far_miss,
                       false, failures);
    }

    // (3) Normalization: reference budgets, cadence independence, clamping,
    // NaN/Inf safety, composite weighting with the raw breakdown intact.
    {
        const bool basic = NormalizeLoadChannel(50.0f, 100.0f, 1.0f) == 0.5f &&
                           NormalizeLoadChannel(25.0f, 100.0f, 0.5f) == 0.5f &&
                           NormalizeLoadChannel(200.0f, 100.0f, 1.0f) == 1.0f &&
                           NormalizeLoadChannel(-5.0f, 100.0f, 1.0f) == 0.0f &&
                           NormalizeLoadChannel(std::numeric_limits<float>::infinity(), 100.0f, 1.0f) ==
                               0.0f &&
                           NormalizeLoadChannel(std::numeric_limits<float>::quiet_NaN(), 100.0f, 1.0f) ==
                               0.0f;
        LoadFieldConfig cfg;
        cfg.simulation_budget = 100.0f;
        cfg.weight_simulation = 0.5f;
        LoadChannels raw;
        raw[LoadChannel::Simulation] = 100.0f; // 100/s -> 1.0
        const NormalizedLoad norm = NormalizeLoad(raw, cfg, 1.0f);
        const bool composite =
            norm[LoadChannel::Simulation] == 1.0f && norm.composite == 0.5f;
        SelftestReport("loadfield-normalization", basic && composite, false, failures);
    }

    // (4) Asymmetric EMA: fast rise leads slow, a one-window spike cannot
    // sustain, sustained load decays after the work stops, predicted == slow.
    {
        LoadFieldConfig cfg; // defaults: fast 1.5/8, slow 8/25
        LoadCell cell;
        LoadChannels raw;
        raw[LoadChannel::Simulation] = 1.0f;
        for (int i = 0; i < 5; ++i) {
            AdvanceLoadCell(cell, raw, cfg, 1.0f);
        }
        const float fast_rise = cell.fast[LoadChannel::Simulation];
        const float slow_rise = cell.slow[LoadChannel::Simulation];
        const bool fast_leads = fast_rise > 0.9f && slow_rise < 0.5f;

        LoadCell spike;
        AdvanceLoadCell(spike, raw, cfg, 1.0f);
        const float spike_peak = spike.fast[LoadChannel::Simulation];
        LoadChannels zero;
        for (int i = 0; i < 5; ++i) {
            AdvanceLoadCell(spike, zero, cfg, 1.0f);
        }
        const bool spike_rejected = spike.fast[LoadChannel::Simulation] < 0.35f &&
                                    spike.slow[LoadChannel::Simulation] < 0.15f &&
                                    spike_peak > 0.4f;

        LoadCell decayed;
        for (int i = 0; i < 5; ++i) {
            AdvanceLoadCell(decayed, raw, cfg, 1.0f);
        }
        for (int i = 0; i < 30; ++i) {
            AdvanceLoadCell(decayed, zero, cfg, 1.0f);
        }
        // After 30 quiet seconds the fast EMA is essentially cold and the
        // slow EMA is well on its way (25s fall tau); fast < slow proves the
        // fast timescale releases the hotspot first.
        const bool decays = decayed.fast[LoadChannel::Simulation] < 0.05f &&
                            decayed.slow[LoadChannel::Simulation] < 0.2f &&
                            decayed.fast[LoadChannel::Simulation] <
                                decayed.slow[LoadChannel::Simulation] &&
                            decayed.predicted[LoadChannel::Simulation] ==
                                decayed.slow[LoadChannel::Simulation];
        std::printf("LOADFIELD smoothing: fast5=%.3f slow5=%.3f spike=[%.3f -> %.3f/%.3f] "
                    "after30=[%.3f/%.3f]\n",
                    fast_rise,
                    slow_rise,
                    spike_peak,
                    spike.fast[LoadChannel::Simulation],
                    spike.slow[LoadChannel::Simulation],
                    decayed.fast[LoadChannel::Simulation],
                    decayed.slow[LoadChannel::Simulation]);
        SelftestReport("loadfield-smoothing", fast_leads && spike_rejected && decays, false,
                       failures);
    }

    // (5) L1 = exact block sum of L0; the grid audit accepts a well-formed
    // generation.
    {
        LoadGrid grid;
        grid.enabled = true;
        grid.cell_size_m = 100.0f;
        grid.bounds = {0.0f, 0.0f, 400.0f, 400.0f};
        grid.dim_x = 4;
        grid.dim_y = 4;
        grid.config.l1_enabled = true;
        grid.config.l1_ratio = 2;
        grid.window_seconds = 1.0f;
        grid.cells.assign(16, LoadCell{});
        grid.cells[0].fast[LoadChannel::AOI] = 1.0f;
        grid.cells[1].fast[LoadChannel::AOI] = 2.0f;
        grid.cells[4].fast[LoadChannel::AOI] = 3.0f;
        grid.cells[15].slow[LoadChannel::Combat] = 7.0f;
        grid.cells[15].predicted[LoadChannel::Combat] = 7.0f; // seam invariant
        RebuildL1(grid);
        grid.active_cells = 4; // the four cells carrying values above
        const bool dims = grid.l1_dim_x == 2 && grid.l1_dim_y == 2 && grid.l1_cells.size() == 4;
        const bool sums = grid.l1_cells[0].fast[LoadChannel::AOI] == 6.0f &&
                          grid.l1_cells[3].slow[LoadChannel::Combat] == 7.0f &&
                          grid.l1_cells[3].predicted[LoadChannel::Combat] == 7.0f;
        std::string error;
        const bool audit = ValidateLoadFieldGrid(grid, error);
        if (!audit) {
            std::printf("LOADFIELD audit error: %s\n", error.c_str());
        }
        SelftestReport("loadfield-l1-and-audit", dims && sums && audit, false, failures);
    }

    // (6) Config validation: every invalid field repaired, never UB.
    {
        LoadFieldConfig bad;
        bad.cell_size_m = -1.0f;
        bad.aggregation_hz = std::numeric_limits<float>::quiet_NaN();
        bad.l1_ratio = 99;
        bad.simulation_budget = 0.0f;
        bad.weight_combat = -2.0f;
        bad.slow_fall_tau_s = 0.0f;
        bad.bounds = WorldBounds{1.0f, 1.0f, 0.0f, 0.0f};
        const auto validated = ValidateLoadFieldConfig(bad);
        const bool repaired =
            !validated.warnings.empty() &&
            validated.effective.cell_size_m == kLoadCellSizeMeters &&
            validated.effective.aggregation_hz == 1.0f &&
            validated.effective.l1_ratio == 4 &&
            validated.effective.simulation_budget == 5000.0f &&
            validated.effective.weight_combat == 1.0f &&
            validated.effective.slow_fall_tau_s == 1.0f && validated.effective.bounds.IsValid();
        std::printf("LOADFIELD config warnings=%zu\n", validated.warnings.size());
        SelftestReport("loadfield-config-validation", repaired, false, failures);
    }

    std::printf("LOADFIELD-SELFTEST-DONE failures=%d\n", failures);
    return failures;
}

bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

// Routing/distribution self-test (§34): unknown + draining destinations,
// stale + duplicate migration delivery, source-free EntityTransfer
// roundtrip. Public runtime APIs only. Returns failure count.
int RunRoutingSelftest(gs::game::WorldRuntime& sim,
                       boost::asio::io_context& io,
                       const BenchConfig& config)
{
    (void)config;
    int failures = 0;
    const auto identity = sim.Identity();

    if (sim.Zones().ZoneCount() < 2) {
        SelftestReport("emulation-needs-2-zones", false, true, failures);
        return failures;
    }
    // Logical-distribution emulation for the draining leg (left on: the
    // subsequent load then runs distributed-emulated, which is intended).
    sim.EmulateDistribution(2);

    // Find an emulated-remote zone.
    std::size_t remote_index = sim.Zones().ZoneCount();
    for (std::size_t i = 0; i < sim.Zones().ZoneCount(); ++i) {
        const auto location = sim.Directory().ResolveZone(sim.Zones().GetZone(i).Id());
        if (location && !sim.Directory().IsLocal(*location)) {
            remote_index = i;
            break;
        }
    }
    if (remote_index >= sim.Zones().ZoneCount()) {
        SelftestReport("emulated-remote-zone", false, true, failures);
        return failures;
    }
    const auto remote_zone_id = sim.Zones().GetZone(remote_index).Id();

    // (a) Unknown destination -> DestinationUnavailable, nothing delivered.
    {
        const auto result = sim.Router().RouteZoneCommand(999999, [](gs::game::Zone&) {});
        SelftestReport("unknown-destination",
                       result == gs::game::DeliveryResult::DestinationUnavailable, false, failures);
    }

    // (b) Draining destination -> DestinationDraining, then recovers.
    {
        sim.Directory().SetZoneDrained(remote_zone_id, true);
        const auto drained =
            sim.Router().RouteZoneCommand(remote_index, [](gs::game::Zone&) {});
        sim.Directory().SetZoneDrained(remote_zone_id, false);
        const auto recovered =
            sim.Router().RouteZoneCommand(remote_index, [](gs::game::Zone&) {});
        SelftestReport("draining-destination",
                       drained == gs::game::DeliveryResult::DestinationDraining &&
                           recovered == gs::game::DeliveryResult::DeliveredRemoteEmulated,
                       false, failures);
    }

    // (c) Stale migration request (bogus net) is dropped, world stays valid.
    {
        const auto zone0 = sim.Zones().GetZone(0).Id();
        sim.TestMigrationQueue().TryEnqueue(
            gs::game::MigrationRequest{42424242u, zone0, remote_zone_id});
        const bool drained = WaitFor(std::chrono::seconds(3),
                                     [&] { return sim.TestMigrationQueue().PendingCount() == 0; });
        std::string error;
        sim.RequestValidation();
        bool valid = false;
        for (int i = 0; i < 50 && !valid; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                valid = (result == "OK");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        SelftestReport("stale-migration-drop", drained && valid, false, failures);
    }

    // (d) Real micro-migration, then exact-id replay must not duplicate.
    // Race-free: the committed id is read from the coordinator AFTER the
    // owner move is observed (LastCommittedId is monotonic per writer).
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), 1);
        sim.PostSpawn(session, MakeBenchCharacter(9000),
                      gs::game::DebugSpawnOverride{460.0f, 200.0f});
        // Wait for the spawn to land so we learn the scout's net id.
        std::uint32_t scout_net = 0;
        WaitFor(std::chrono::seconds(5), [&] {
            const auto it = sim.Owners().find(1);
            if (it == sim.Owners().end()) {
                return false;
            }
            scout_net = it->second.net_id;
            return true;
        });
        bool migrated = false;
        std::size_t home_index = sim.Zones().ZoneCount();
        std::uint32_t home_zone_id = 0;
        if (scout_net != 0) {
            const auto home = sim.Owners().find(1);
            home_index = home != sim.Owners().end() ? home->second.zone_index : home_index;
            home_zone_id = home_index < sim.Zones().ZoneCount()
                               ? sim.Zones().GetZone(home_index).Id()
                               : 0;
            std::uint32_t seq = 0;
            const auto move_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < move_deadline && !migrated) {
                sim.PostMoveInput(1, ++seq, 1.5707963f, gs::game::MoveState::Running);
                const auto owner = sim.Owners().find(1);
                if (owner != sim.Owners().end() && owner->second.zone_index != home_index) {
                    migrated = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        bool replay_safe = false;
        if (migrated) {
            // Replay the just-committed id against the ORIGINAL route. The
            // committed-set must drop it (duplicates+1) even though the
            // entity still exists -- no second entity may appear.
            const auto owner = sim.Owners().find(1);
            const std::uint32_t dst_zone_id =
                owner != sim.Owners().end() && owner->second.zone_index < sim.Zones().ZoneCount()
                    ? sim.Zones().GetZone(owner->second.zone_index).Id()
                    : 0;
            const auto committed_id = sim.LastCommittedMigration();
            const auto dup_before = sim.MigrationMetrics().duplicates;
            gs::game::MigrationRequest replay{};
            replay.net_id = scout_net;
            replay.source_zone_id = home_zone_id;
            replay.target_zone_id = dst_zone_id;
            replay.migration_id = committed_id;
            replay.priority = gs::game::MessagePriority::Critical;
            // The net may legitimately be re-queued already (still out of
            // bounds in the new zone); retry briefly, else SKIP -- forcing
            // the test would conflate load with incorrectness.
            bool enqueued = false;
            if (committed_id.IsValid() && dst_zone_id != 0) {
                const auto enqueue_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < enqueue_deadline && !enqueued) {
                    enqueued = sim.TestMigrationQueue().TryEnqueue(replay);
                    if (!enqueued) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }
            if (!enqueued) {
                sim.PostDespawn(1);
                SelftestReport("duplicate-migration-id", false, true, failures);
            } else {
                // PendingCount==0 only means dequeued; the duplicate counter
                // proves the committed-set actually took the drop.
                WaitFor(std::chrono::seconds(3), [&] {
                    return sim.MigrationMetrics().duplicates == dup_before + 1;
                });
                const auto after = sim.Owners().find(1);
                const auto dup_after = sim.MigrationMetrics().duplicates;
                replay_safe = after != sim.Owners().end() &&
                              after->second.zone_index != home_index &&
                              dup_after == dup_before + 1;
                std::string error;
                sim.RequestValidation();
                for (int i = 0; i < 50; ++i) {
                    std::string result;
                    if (sim.TryTakeValidationResult(result)) {
                        replay_safe = replay_safe && (result == "OK");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                sim.PostDespawn(1);
                SelftestReport("duplicate-migration-id", replay_safe, false, failures);
            }
        } else {
            sim.PostDespawn(1);
            SelftestReport("duplicate-migration-id", false, true, failures);
        }

        // (e) Source-free DTO roundtrip on the (possibly migrated) scout.
        bool roundtrip = false;
        if (scout_net != 0) {
            const auto owner = sim.Owners().find(1);
            if (owner != sim.Owners().end() && owner->second.zone_index < sim.Zones().ZoneCount()) {
                const auto entity =
                    sim.Zones().GetZone(owner->second.zone_index).FindEntity(scout_net);
                if (entity.is_valid()) {
                    auto transfer = gs::game::BuildTransfer(entity, true, 0);
                    const gs::game::EntityTransfer wire_copy = transfer; // simulated transport
                    roundtrip = wire_copy.net_id == scout_net && wire_copy.is_player &&
                                wire_copy.session == 1 &&
                                wire_copy.position.x == transfer.position.x &&
                                wire_copy.hp.current == transfer.hp.current;
                    flecs::world scratch;
                    const auto rebuilt = gs::game::ApplyTransfer(scratch, wire_copy);
                    roundtrip =
                        roundtrip && rebuilt.is_valid() &&
                        rebuilt.get<gs::game::NetId>().value == scout_net &&
                        rebuilt.get<gs::game::Position>().x == transfer.position.x &&
                        rebuilt.has<gs::game::PlayerTag>();
                }
            }
        }
        SelftestReport("transfer-roundtrip", roundtrip, scout_net == 0, failures);
    }

    return failures;
}

// Deterministic split -> merge transaction scenario (§16-18). Own sim
// lifecycle; does NOT use the main load loop above.
//
//   1. ConfigurePartition (min_zone 100m so the 500m test-map zones can
//      split; everything else default) + verify effective config.
//   2. Spawn 8 players + ~20 mobs concentrated in one zone; 3 more players
//      exactly on the zone's future child midlines (boundary semantics).
//   3. ForceSplit -> expect COMMIT (or ABORT when --fail-* armed):
//      topology, totals, boundary placement, validator.
//   4. Despawn all players, ForceMerge -> merged holds all mobs, children
//      retired+empty, validator.
// Returns failure count (0 = PASS).
int RunSplitMergeScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("SPLITMERGE %s: PASS\n", name);
        } else {
            std::printf("SPLITMERGE %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("SPLITMERGE validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("SPLITMERGE validation(%s): TIMEOUT\n", what);
        return false;
    };
    auto active_leaf_ids = [&]() {
        std::set<gs::game::ZoneId> ids;
        for (const auto* leaf : sim.Zones().GetActiveLeaves()) {
            ids.insert(leaf->zone_id);
        }
        return ids;
    };
    auto find_zone = [&](gs::game::ZoneId id) -> const gs::game::Zone* {
        const std::size_t idx = sim.Zones().FindIndexById(id);
        return idx < sim.Zones().ZoneCount() ? &sim.Zones().GetZone(idx) : nullptr;
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // ---- 1. config ----
    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f; // test map zones are 500m; halves must clear the floor
    sim.ConfigurePartition(pcfg);
    // 100m is below the 2x AOI safety floor (240m): validation must clamp
    // it, and the clamped value is what the split machinery runs on. This
    // doubles as an end-to-end config-validation check.
    check("config-effective",
          sim.EffectivePartitionConfig().min_zone_size_m == 240.0f &&
              sim.EffectivePartitionConfig().split_load_threshold == 0.9f);
    sim.Start();

    // ---- 2. populate one zone ----
    const auto& z0 = sim.Zones().GetZone(0);
    const float cx = (z0.Bounds().min_x + z0.Bounds().max_x) * 0.5f;
    const float cy = (z0.Bounds().min_y + z0.Bounds().max_y) * 0.5f;
    constexpr int kPlayers = 8;
    constexpr int kBoundaryPlayers = 3;
    constexpr int kTotalPlayers = kPlayers + kBoundaryPlayers;
    constexpr gs::common::SessionId kBaseSession = 500;
    std::vector<std::shared_ptr<gs::network::Session>> sessions;
    for (int i = 0; i < kPlayers; ++i) {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(
            std::move(socket), static_cast<gs::common::SessionId>(kBaseSession + i));
        sessions.push_back(session);
        sim.PostSpawn(session,
                      MakeBenchCharacter(700 + i),
                      gs::game::DebugSpawnOverride{cx + static_cast<float>(i),
                                                   cy + static_cast<float>(i % 3)});
    }
    constexpr int kMobPoints = 20;
    for (int k = 0; k < kMobPoints; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = cx;
        point.y = cy;
        point.count = 1;
        point.radius = 2.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < kMobPoints; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kPlayers) &&
               sim.CollectProcessLoad().mobs >= 16;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    const std::uint64_t mob_total = sim.CollectProcessLoad().mobs;

    // Target = wherever the spawns actually landed (no map assumptions).
    const gs::game::ZoneId target_id = sim.Owners().at(kBaseSession).location.zone;
    bool all_in_target = true;
    for (int i = 0; i < kPlayers; ++i) {
        const auto it = sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + i));
        if (it == sim.Owners().end() || it->second.location.zone != target_id) {
            all_in_target = false;
        }
    }
    check("single-target-zone", all_in_target);
    const auto* target_zone = find_zone(target_id);
    if (target_zone == nullptr) {
        sim.Stop();
        return failures + 1;
    }
    const auto tb = target_zone->Bounds();
    const float mid_x = (tb.min_x + tb.max_x) * 0.5f;
    const float mid_y = (tb.min_y + tb.max_y) * 0.5f;

    // Boundary players exactly on future child midlines (§23).
    struct BoundarySpawn {
        float x, y;
    };
    const BoundarySpawn kBoundary[kBoundaryPlayers] = {
        {mid_x, (tb.min_y + mid_y) * 0.5f}, // vertical midline -> east child
        {(tb.min_x + mid_x) * 0.5f, mid_y}, // horizontal midline -> north child
        {mid_x, mid_y},                     // corner shared by 4 -> north-east child
    };
    for (int i = 0; i < kBoundaryPlayers; ++i) {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(
            std::move(socket), static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
        sessions.push_back(session);
        sim.PostSpawn(session,
                      MakeBenchCharacter(710 + i),
                      gs::game::DebugSpawnOverride{kBoundary[i].x, kBoundary[i].y});
    }
    populated = WaitFor(std::chrono::seconds(15), [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kTotalPlayers);
    });
    check("boundary-spawn", populated && !expired());

    // Boundary spawns must have landed inside the target (interior points).
    bool boundary_home = true;
    std::vector<std::uint32_t> boundary_nets;
    for (int i = 0; i < kBoundaryPlayers; ++i) {
        const auto it = sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
        if (it == sim.Owners().end() || it->second.location.zone != target_id) {
            boundary_home = false;
        } else {
            boundary_nets.push_back(it->second.net_id);
        }
    }
    check("boundary-in-target", boundary_home);
    check("pre-split-validate", validate_now("pre-split"));

    const auto pre_leaves = active_leaf_ids();
    const bool expect_abort = config.fail_snapshot > 0 || config.fail_apply > 0;
    if (expect_abort) {
        sim.InjectTransferFailuresForTest(config.fail_snapshot, config.fail_apply,
                                          config.fail_after);
    }

    // ---- 3. forced split ----
    sim.PostForceSplit(target_id);
    const auto split0 = sim.PartitionMetricsSnapshot();
    const bool split_settled = WaitFor(std::chrono::seconds(25), [&] {
        const auto m = sim.PartitionMetricsSnapshot();
        return m.split_commits + m.split_aborts > split0.split_commits + split0.split_aborts;
    });
    check("split-settled", split_settled && !expired());
    const auto split1 = sim.PartitionMetricsSnapshot();
    check("pre-merge-validate-split", validate_now("post-split"));

    auto totals_ok = [&] {
        return sim.Owners().size() == static_cast<std::size_t>(kTotalPlayers) &&
               sim.CollectProcessLoad().mobs == mob_total;
    };
    if (expect_abort) {
        check("split-aborted",
              split1.split_aborts > split0.split_aborts &&
                  split1.split_commits == split0.split_commits);
        // Parent restored as the sole authority; nothing half-built.
        const auto leaves = active_leaf_ids();
        check("abort-parent-authority", leaves.find(target_id) != leaves.end());
        check("abort-no-loss", totals_ok());
        // Apply-stage failure additionally proves destination rollback: the
        // retired-tombstone rule would fail validation otherwise (checked).
    } else {
        check("split-committed",
              split1.split_commits > split0.split_commits &&
                  split1.split_aborts == split0.split_aborts);
        const auto leaves = active_leaf_ids();
        std::set<gs::game::ZoneId> new_children;
        for (auto id : leaves) {
            if (pre_leaves.find(id) == pre_leaves.end()) {
                new_children.insert(id);
            }
        }
        check("split-4-children", new_children.size() == 4);
        check("split-parent-retired",
              leaves.find(target_id) == leaves.end() &&
                  [&] {
                      const auto* z = find_zone(target_id);
                      return z != nullptr &&
                             z->Partition() == gs::game::PartitionState::Retired &&
                             z->Entities().empty() && z->Players().empty();
                  }());
        check("split-no-loss", totals_ok());
        // Boundary placement: each midline player in exactly the half-open
        // owner child (§23).
        bool placement_ok = new_children.size() == 4;
        for (int i = 0; i < kBoundaryPlayers && placement_ok; ++i) {
            const auto it =
                sim.Owners().find(static_cast<gs::common::SessionId>(kBaseSession + kPlayers + i));
            if (it == sim.Owners().end()) {
                placement_ok = false;
                break;
            }
            const float px = kBoundary[i].x;
            const float py = kBoundary[i].y;
            int matches = 0;
            gs::game::ZoneId expected = 0;
            for (auto cid : new_children) {
                const auto* cz = find_zone(cid);
                if (cz == nullptr) {
                    continue;
                }
                const auto& b = cz->Bounds();
                if (px >= b.min_x && px < b.max_x && py >= b.min_y && py < b.max_y) {
                    ++matches;
                    expected = cid;
                }
            }
            placement_ok = (matches == 1 && it->second.location.zone == expected);
        }
        check("split-boundary-placement", placement_ok);
    }

    // ---- 4. despawn players, forced merge ----
    // In abort mode there are no children to merge (the split never
    // committed): the merge phase is skipped, topology must simply be the
    // untouched original plus validation + metrics.
    if (expect_abort) {
        check("abort-post-validate", validate_now("post-abort"));
        const auto am = sim.PartitionMetricsSnapshot();
        check("abort-metrics",
              am.split_aborts >= 1 && am.split_commits == 0 && am.merge_attempts == 0);
    }
    for (int i = 0; i < kTotalPlayers; ++i) {
        sim.PostDespawn(static_cast<gs::common::SessionId>(kBaseSession + i));
    }
    sessions.clear();
    const bool drained = WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-drained", drained && !expired());
    if (expect_abort) {
        check("abort-final-validate", validate_now("final-abort"));
        sim.Stop();
        std::printf("SPLITMERGE-DONE validations=%d failures=%d\n", validations, failures);
        return failures;
    }

    sim.PostForceMerge(target_id);
    const auto merge0 = sim.PartitionMetricsSnapshot();
    const bool merge_settled = WaitFor(std::chrono::seconds(25), [&] {
        const auto m = sim.PartitionMetricsSnapshot();
        return m.merge_commits + m.merge_aborts > merge0.merge_commits + merge0.merge_aborts;
    });
    check("merge-settled", merge_settled && !expired());
    const auto merge1 = sim.PartitionMetricsSnapshot();
    check("merge-committed", merge1.merge_commits > merge0.merge_commits);
    check("post-merge-validate", validate_now("post-merge"));

    // Merged leaf active; all split children retired+empty; mobs preserved.
    const auto post_leaves = active_leaf_ids();
    std::set<gs::game::ZoneId> merged_ids;
    for (auto id : post_leaves) {
        if (pre_leaves.find(id) == pre_leaves.end()) {
            merged_ids.insert(id);
        }
    }
    // Note: with expect_abort the split never committed, so there are no
    // children to merge; the forced merge then operates on whatever the
    // tree holds (likely a no-op plan refusal). Merge asserts below only
    // apply to the commit path.
    if (!expect_abort) {
        check("merge-single-target", merged_ids.size() == 1);
        std::uint64_t total_mobs = 0;
        bool children_clean = true;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& z = sim.Zones().GetZone(zi);
            total_mobs += z.Entities().size();
            // Mobs may have wandered out of the merged subtree into
            // neighbors; the invariant is global preservation + retired
            // emptiness, not that every mob sits in the merged zone.
            if (z.Partition() == gs::game::PartitionState::Retired) {
                if (!z.Entities().empty() || !z.Players().empty()) {
                    children_clean = false;
                }
            }
        }
        check("merge-children-retired-empty", children_clean);
        check("merge-mobs-preserved",
              total_mobs == mob_total && sim.CollectProcessLoad().mobs == mob_total);
    }

    const auto pm = sim.PartitionMetricsSnapshot();
    std::printf("SPLITMERGE metrics: split att=%llu ok=%llu ab=%llu tf=%llu rb=%llu "
                "merge att=%llu ok=%llu ab=%llu tf=%llu rb=%llu retire_rej=%llu\n",
                (unsigned long long)pm.split_attempts, (unsigned long long)pm.split_commits,
                (unsigned long long)pm.split_aborts, (unsigned long long)pm.split_transfer_failures,
                (unsigned long long)pm.split_rollback_failures,
                (unsigned long long)pm.merge_attempts, (unsigned long long)pm.merge_commits,
                (unsigned long long)pm.merge_aborts, (unsigned long long)pm.merge_transfer_failures,
                (unsigned long long)pm.merge_rollback_failures,
                (unsigned long long)pm.retire_rejected_nonempty);
    auto avg = [](std::uint64_t total, std::uint64_t n) {
        return n > 0 ? static_cast<double>(total) / n / 1000.0 : 0.0;
    };
    std::printf("SPLITMERGE phase ms avg: split plan=%.3f xfer=%.3f commit=%.3f rollback=%.3f | "
                "merge plan=%.3f xfer=%.3f commit=%.3f rollback=%.3f\n",
                avg(pm.split_plan_us, pm.split_commits + pm.split_aborts),
                avg(pm.split_transfer_us, pm.split_commits + pm.split_aborts),
                avg(pm.split_commit_us, pm.split_commits),
                avg(pm.split_rollback_us, pm.split_aborts),
                avg(pm.merge_plan_us, pm.merge_commits + pm.merge_aborts),
                avg(pm.merge_transfer_us, pm.merge_commits + pm.merge_aborts),
                avg(pm.merge_commit_us, pm.merge_commits),
                avg(pm.merge_rollback_us, pm.merge_aborts));

    sim.Stop();
    std::printf("SPLITMERGE-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Simulation LOD correctness scenario (§26). Own sim lifecycle; default LOD
// config (enabled). All asserts use thread-safe public reads (diagnostic
// gauges, Owners, validator, cumulative counters) — never live flecs state.
//
//   Phase 1: 1 AFK player + 2 mob clusters (near/far). Far cluster must
//     cascade to Dormant; near cluster stays Full; validator OK.
//   Phase 2: spawn a player at the far cluster -> Full jumps within ~2 eval
//     periods (immediate proximity wake).
//   Phase 3: blind attack burst around the far cluster -> deaths happen on
//     previously-dormant mobs (damage path transparent under LOD),
//     validator OK. (Proximity bubbles do most waking; the synchronous
//     attack Wake is the sub-eval-period guarantee — both are correct.)
//   Phase 4: despawn everyone, wait out the cascade -> Full decays to zero
//     (hysteresis, no flap); cooldowns/respawns advanced meanwhile.
// Returns failure count (0 = PASS).
int RunLodScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("LOD %s: PASS\n", name);
        } else {
            std::printf("LOD %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("LOD validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("LOD validation(%s): TIMEOUT\n", what);
        return false;
    };
    struct TierDist {
        std::uint64_t full = 0, reduced = 0, low = 0, dormant = 0;
    };
    auto tiers = [&]() {
        TierDist dist;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
            dist.full += diag.lod_full.load(std::memory_order_relaxed);
            dist.reduced += diag.lod_reduced.load(std::memory_order_relaxed);
            dist.low += diag.lod_low.load(std::memory_order_relaxed);
            dist.dormant += diag.lod_dormant.load(std::memory_order_relaxed);
        }
        return dist;
    };
    auto print_zones = [&](const char* tag) {
        std::printf("LOD zones@%s:\n", tag);
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& z = sim.Zones().GetZone(zi);
            const auto& diag = z.Diagnostics();
            const auto& b = z.Bounds();
            std::printf("  zone=%u bounds=(%.0f,%.0f)-(%.0f,%.0f) mobs=%u tiers=[%u/%u/%u/%u]\n",
                        z.Id(),
                        b.min_x,
                        b.min_y,
                        b.max_x,
                        b.max_y,
                        diag.mob_count.load(std::memory_order_relaxed),
                        diag.lod_full.load(std::memory_order_relaxed),
                        diag.lod_reduced.load(std::memory_order_relaxed),
                        diag.lod_low.load(std::memory_order_relaxed),
                        diag.lod_dormant.load(std::memory_order_relaxed));
        }
        for (const auto& [sid, owner] : sim.Owners()) {
            std::printf("  owner session=%llu zone=%u net=%u\n",
                        (unsigned long long)sid,
                        owner.location.zone,
                        owner.net_id);
        }
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(150, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    sim.Start();

    // ---- Phase 1: populate. Group A near (150,150), group B near (850,850),
    // player 1 AFK at group A. Mobs spawn via points (type 2) around both.
    constexpr gs::common::SessionId kPlayer1 = 600;
    constexpr gs::common::SessionId kPlayer2 = 601;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer1);
        sim.PostSpawn(session, MakeBenchCharacter(800), gs::game::DebugSpawnOverride{150.0f, 150.0f});
    }
    constexpr int kMobPoints = 12;
    for (int k = 0; k < kMobPoints; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        if (k < kMobPoints / 2) {
            point.x = 150.0f;
            point.y = 150.0f;
        } else {
            point.x = 850.0f;
            point.y = 850.0f;
        }
        point.count = 1;
        point.radius = 3.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < kMobPoints; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    const bool populated = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 1 && sim.CollectProcessLoad().mobs >= 10;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    const std::uint64_t mob_total = sim.CollectProcessLoad().mobs;
    print_zones("populated");
    std::printf("LOD lookup: (150,150)->zoneidx=%zu (850,850)->zoneidx=%zu\n",
                sim.Zones().FindIndexForPosition(150.0f, 150.0f),
                sim.Zones().FindIndexForPosition(850.0f, 850.0f));
    check("pre-validate", validate_now("pre"));
    auto tiers_sum = [](const TierDist& d) { return d.full + d.reduced + d.low + d.dormant; };

    // ---- Phase 1: dormancy. Newborns carry no grace history, so group B
    // (far from the only player) cascades to Dormant within ~3 evals, while
    // group A stays Full. Wait generously, then assert distribution shape,
    // exact tier accounting (every mob in exactly one tier) and no loss.
    std::this_thread::sleep_for(std::chrono::seconds(20));
    if (expired()) {
        check("dormancy-window", false);
        sim.Stop();
        return failures + 1;
    }
    const TierDist d1 = tiers();
    std::printf("LOD tiers@t+20s: full=%llu reduced=%llu low=%llu dormant=%llu (mobs=%llu)\n",
                (unsigned long long)d1.full,
                (unsigned long long)d1.reduced,
                (unsigned long long)d1.low,
                (unsigned long long)d1.dormant,
                (unsigned long long)mob_total);
    check("near-full", d1.full >= 3);
    check("far-demoted", d1.dormant + d1.low + d1.reduced >= 3);
    check("tier-accounting-1", tiers_sum(d1) == mob_total);
    check("no-loss-phase1", sim.CollectProcessLoad().mobs == mob_total);
    check("validate-phase1", validate_now("phase1"));

    // ---- Phase 2: leave hysteresis (before any death, so no respawn can
    // flake the asserts). Player 1 despawns; Full must decay after the 5s
    // grace (Reduced), with no flap back while nobody is around.
    sim.PostDespawn(kPlayer1);
    const bool left1 =
        WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-1-drained", left1 && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(12));
    if (expired()) {
        check("hysteresis-window", false);
        sim.Stop();
        return failures + 1;
    }
    const TierDist d2 = tiers();
    std::printf("LOD tiers@left+12s: full=%llu reduced=%llu low=%llu dormant=%llu\n",
                (unsigned long long)d2.full,
                (unsigned long long)d2.reduced,
                (unsigned long long)d2.low,
                (unsigned long long)d2.dormant);
    check("hysteresis-decay", d2.full == 0);
    check("tier-accounting-2", tiers_sum(d2) == mob_total);
    check("validate-phase2", validate_now("phase2"));

    // ---- Phase 3: wake on approach. Player 2 spawns at group B; Full must
    // jump within ~2 evaluation periods without any attack input.
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer2);
        sim.PostSpawn(session, MakeBenchCharacter(801), gs::game::DebugSpawnOverride{850.0f, 850.0f});
    }
    const bool woke = WaitFor(std::chrono::seconds(10), [&] { return tiers().full >= 5; });
    print_zones("after-approach");
    check("wake-on-approach", woke && !expired());
    check("validate-phase3", validate_now("phase3"));

    // ---- Phase 4: attacks land (damage path transparent under LOD).
    // Blind burst over the mob net range; deaths prove hits. Proximity
    // bubbles do most waking; the synchronous attack Wake is the
    // sub-eval-period guarantee (wakes counter reported, not asserted:
    // attribution between the two correct paths is timing-dependent).
    const std::uint64_t deaths_before = sim.DeathsTotal();
    const auto attack_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < attack_until && !expired()) {
        for (std::uint32_t net = 1000000; net < 1000060; ++net) {
            sim.PostAttackTarget(kPlayer2, net);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const std::uint64_t deaths = sim.DeathsTotal() - deaths_before;
    const auto work = sim.LodWorkTotalsSnapshot();
    std::printf("LOD combat: deaths=%llu wakes=%llu\n",
                (unsigned long long)deaths,
                (unsigned long long)work.wakes);
    check("attack-kills", deaths > 0);
    // Deaths remove, respawns (30s timers) add back: conservation means the
    // count stays within [total-deaths, total], never below (loss) or above
    // (duplication).
    {
        const std::uint64_t mobs_now = sim.CollectProcessLoad().mobs;
        std::printf("LOD mobs: total=%llu deaths=%llu now=%llu\n",
                    (unsigned long long)mob_total,
                    (unsigned long long)deaths,
                    (unsigned long long)mobs_now);
        check("no-loss-phase4", mobs_now <= mob_total && mobs_now >= mob_total - deaths);
    }
    check("validate-phase4", validate_now("phase4"));

    // ---- Phase 5: respawn timers advance under LOD (supervisor-side
    // countdowns are tier-independent by design). Deaths from phase 4 must
    // come back within their window.
    const bool respawned = WaitFor(std::chrono::seconds(45), [&] {
        return sim.CollectProcessLoad().mobs >= mob_total;
    });
    check("respawn-advance", respawned && !expired());
    check("validate-phase5", validate_now("phase5"));

    const auto work_end = sim.LodWorkTotalsSnapshot();
    std::printf("LOD work totals: ai=%llu mv=%llu prom=%llu dem=%llu wakes=%llu eval_us=%llu\n",
                (unsigned long long)work_end.ai_updates,
                (unsigned long long)work_end.move_updates,
                (unsigned long long)work_end.promotions,
                (unsigned long long)work_end.demotions,
                (unsigned long long)work_end.wakes,
                (unsigned long long)work_end.eval_us);

    sim.Stop();
    std::printf("LOD-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Cross-zone LOD determinism via the Spatial Activity Field (§32). Own sim
// lifecycle. Uses SMALL LOD radii (15/50/150, short graces) configured
// pre-Start, so every tier boundary fits inside the 1km test map while the
// comparison machinery stays identical (config stays authority).
//
//   Phase 1: discover an adjacent zone pair (A,B) from live bounds. Player
//     in A near the shared edge; 3 static mobs in B at 12/40/110m.
//     Expect exactly Full/Reduced/Low, all flagged cross-zone.
//   Phase 2: 9 more static mobs at 14/15/16, 49/50/51, 139/140/141m.
//     Expect F/R/R, R/L/L, L/Dormant/Dormant (half-open boundaries exact).
//   Phase 3: despawn the player; everything must reach Dormant (fast
//     cascade with the short test graces); a zone must fall asleep;
//     validator Dormant rules execute live.
//   Phase 4: spawn a player OUTSIDE the sleeping zone but inside the wake
//     radius: the zone must wake without any entry (predictive wake).
// Static mobs (spawn radius 0) never move, the AFK player never moves:
// positions are bit-exact forever, so asserts are deterministic.
// Returns failure count (0 = PASS).
int RunActivityScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("ACTIVITY %s: PASS\n", name);
        } else {
            std::printf("ACTIVITY %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("ACTIVITY validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY validation(%s): TIMEOUT\n", what);
        return false;
    };
    // Detailed field audit + per-mob samples matched by EXACT position
    // (static entities: bit-exact forever, no live flecs reads from here).
    auto activity_samples = [&](const char* what, std::size_t max_samples,
                                std::vector<gs::game::ActivitySampleResult>& out) -> bool {
        sim.RequestActivityValidation(max_samples);
        for (int i = 0; i < 100; ++i) {
            std::string error;
            if (sim.TryTakeActivitySamples(out, error)) {
                ++validations;
                return true;
            }
            if (!error.empty()) {
                ++validations;
                std::printf("ACTIVITY detailed(%s): FAIL: %s\n", what, error.c_str());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY detailed(%s): TIMEOUT\n", what);
        return false;
    };
    auto find_sample = [](const std::vector<gs::game::ActivitySampleResult>& samples, float x,
                          float y) -> const gs::game::ActivitySampleResult* {
        for (const auto& sample : samples) {
            if (sample.x == x && sample.y == y) {
                return &sample;
            }
        }
        return nullptr;
    };

    // Small radii so every boundary fits in-map; short graces so the
    // Dormant leg stays fast. Same machinery, config-driven (§5).
    gs::game::LodConfig test_lod;
    test_lod.full_radius_m = 15.0f;
    test_lod.reduced_radius_m = 50.0f;
    test_lod.low_radius_m = 150.0f;
    test_lod.demote_full_sec = 2.0f;
    test_lod.demote_reduced_sec = 2.0f;
    test_lod.demote_low_sec = 2.0f;
    sim.ConfigureSimulationLod(test_lod);
    if (sim.EffectiveLodConfig().low_radius_m != 150.0f) {
        check("config-authority", false);
        return failures + 1;
    }
    check("config-authority", true);
    sim.Start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(120, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // ---- Topology discovery: adjacent pair (A,B) sharing an x-edge with
    // >=100m y-overlap, read from LIVE bounds (robust to splits, of which
    // none can happen at this load anyway).
    float x_edge = 0.0f, y_mid = 0.0f;
    std::size_t zone_a = 0, zone_b = 0;
    bool have_pair = false;
    {
        const auto& zones = sim.Zones();
        for (std::size_t i = 0; i < zones.ZoneCount() && !have_pair; ++i) {
            for (std::size_t j = 0; j < zones.ZoneCount() && !have_pair; ++j) {
                if (i == j) {
                    continue;
                }
                const auto& a = zones.GetZone(i).Bounds();
                const auto& b = zones.GetZone(j).Bounds();
                const float overlap =
                    std::min(a.max_y, b.max_y) - std::max(a.min_y, b.min_y);
                if (std::abs(a.max_x - b.min_x) < 0.01f && overlap >= 100.0f) {
                    zone_a = i;
                    zone_b = j;
                    x_edge = a.max_x;
                    y_mid = (std::max(a.min_y, b.min_y) + std::min(a.max_y, b.max_y)) * 0.5f;
                    have_pair = true;
                }
            }
        }
    }
    check("adjacent-pair", have_pair && !expired());
    if (!have_pair) {
        sim.Stop();
        return failures + 1;
    }
    std::printf("ACTIVITY edge: A=%zu B=%zu x=%.0f ymid=%.0f\n", zone_a, zone_b, x_edge, y_mid);
    const float px = x_edge - 10.0f; // player 10m inside A

    // ---- Phase 1: player + 3 static mobs across the border.
    constexpr gs::common::SessionId kPlayer = 700;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer);
        sim.PostSpawn(session, MakeBenchCharacter(900), gs::game::DebugSpawnOverride{px, y_mid});
    }
    const float kDistances[3] = {12.0f, 40.0f, 110.0f}; // Full / Reduced / Low
    for (int k = 0; k < 3; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = px + kDistances[k];
        point.y = y_mid;
        point.count = 1;
        point.radius = 0.0f; // static mob: exact position forever
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < 3; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    const bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 1 && sim.CollectProcessLoad().mobs >= 3;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    // Placement proof via position->zone mapping (no live flecs reads):
    // player in A, every mob in B.
    bool placed = sim.Zones().FindIndexForPosition(px, y_mid) == zone_a;
    for (int k = 0; k < 3 && placed; ++k) {
        placed = sim.Zones().FindIndexForPosition(px + kDistances[k], y_mid) == zone_b;
    }
    check("cross-zone-placement", placed);
    check("pre-validate", validate_now("pre"));
    std::this_thread::sleep_for(std::chrono::seconds(6)); // eval + settle
    if (expired()) {
        check("settle-window", false);
        sim.Stop();
        return failures + 1;
    }
    std::vector<gs::game::ActivitySampleResult> samples;
    bool detailed_ok = activity_samples("distances", 64, samples);
    check("detailed-ok", detailed_ok);
    const gs::game::SimulationTier kExpected1[3] = {
        gs::game::SimulationTier::Full,
        gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Low,
    };
    bool tiers_ok = detailed_ok;
    for (int k = 0; k < 3 && tiers_ok; ++k) {
        const auto* sample = find_sample(samples, px + kDistances[k], y_mid);
        if (sample == nullptr || sample->field_tier != kExpected1[k] ||
            sample->brute_tier != kExpected1[k]) {
            std::printf("ACTIVITY distance[%d]: %s (expected %d)\n",
                        k,
                        sample == nullptr ? "sample-missing"
                                          : (sample->field_tier != sample->brute_tier ? "field!=brute"
                                                                                      : "wrong-tier"),
                        static_cast<int>(kExpected1[k]));
            tiers_ok = false;
        }
    }
    check("cross-zone-tiers", tiers_ok);
    // Metric path: every grant above came from the foreign zone's player,
    // so the cross-zone counters must observe traffic (accumulated over a
    // few diag windows to be independent of exchange phasing).
    {
        std::uint64_t seen = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until && seen == 0 && !expired()) {
            for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
                const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
                seen += diag.cross_zone_full_since_diag.load(std::memory_order_relaxed);
                seen += diag.cross_zone_reduced_since_diag.load(std::memory_order_relaxed);
                seen += diag.cross_zone_low_since_diag.load(std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("ACTIVITY cross-counters observed: %llu\n", (unsigned long long)seen);
        check("cross-counters-live", seen > 0);
    }
    check("validate-distances", validate_now("distances"));

    // ---- Phase 2: half-open boundary exactness (same player, 9 mobs).
    const float kBounds[9] = {14.0f, 15.0f, 16.0f, 49.0f, 50.0f,  51.0f,
                              149.0f, 150.0f, 151.0f};
    const gs::game::SimulationTier kExpected2[9] = {
        gs::game::SimulationTier::Full,    gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Reduced, gs::game::SimulationTier::Reduced,
        gs::game::SimulationTier::Low,     gs::game::SimulationTier::Low,
        gs::game::SimulationTier::Low,     gs::game::SimulationTier::Dormant,
        gs::game::SimulationTier::Dormant,
    };
    for (int k = 0; k < 9; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = px + kBounds[k];
        point.y = y_mid;
        point.count = 1;
        point.radius = 0.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < 9; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(7 + k));
    }
    const bool populated2 = WaitFor(std::chrono::seconds(25), [&] {
        return sim.CollectProcessLoad().mobs >= 12;
    });
    check("boundary-populate", populated2 && !expired());
    if (!populated2) {
        sim.Stop();
        return failures + 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    if (expired()) {
        check("boundary-window", false);
        sim.Stop();
        return failures + 1;
    }
    std::vector<gs::game::ActivitySampleResult> samples2;
    bool detailed2 = activity_samples("boundaries", 64, samples2);
    check("detailed2-ok", detailed2);
    bool bounds_ok = detailed2;
    for (int k = 0; k < 9 && bounds_ok; ++k) {
        const auto* sample = find_sample(samples2, px + kBounds[k], y_mid);
        if (sample == nullptr || sample->field_tier != kExpected2[k] ||
            sample->brute_tier != kExpected2[k]) {
            std::printf("ACTIVITY boundary[%d] d=%.0f: %s (expected %d)\n",
                        k,
                        kBounds[k],
                        sample == nullptr ? "sample-missing"
                                          : (sample->field_tier != sample->brute_tier ? "field!=brute"
                                                                                      : "wrong-tier"),
                        static_cast<int>(kExpected2[k]));
            bounds_ok = false;
        }
    }
    check("boundary-exactness", bounds_ok);
    check("validate-boundaries", validate_now("boundaries"));
    // No topology churn allowed at this load (determinism guard).
    check("topology-stable", sim.Zones().ZoneCount() == 3);

    // ---- Phase 3: despawn -> Full/Reduced work must drain (sleep
    // precondition), zones fall asleep, validator Dormant rules execute
    // live on the already-Dormant boundary mobs. NOTE: remaining Low mobs
    // freeze mid-cascade in sleeping zones BY DESIGN (no ticks = no eval);
    // they resume and finish demoting on wake. Asserting all-Dormant here
    // would contradict the sleep economy, so assert no-Full/Reduced.
    sim.PostDespawn(kPlayer);
    const bool drained = WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("despawn-drained", drained && !expired());
    const bool quiet = WaitFor(std::chrono::seconds(20), [&] {
        std::uint64_t hot = 0, total = 0;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
            hot += diag.lod_full.load(std::memory_order_relaxed) +
                   diag.lod_reduced.load(std::memory_order_relaxed);
            total += hot + diag.lod_low.load(std::memory_order_relaxed) +
                     diag.lod_dormant.load(std::memory_order_relaxed);
        }
        return total >= 12 && hot == 0;
    });
    check("no-full-reduced", quiet && !expired());
    check("validate-dormant", validate_now("dormant"));
    const bool slept = WaitFor(std::chrono::seconds(10), [&] {
        return sim.CollectProcessLoad().sleeping_zones >= 1;
    });
    check("zone-slept", slept && !expired());

    // ---- Phase 4: predictive wake. Spawn OUTSIDE the sleeping zone but
    // inside the wake radius (reduced 50m): the zone must wake with no
    // entry, then validate.
    const float wake_x = x_edge - 40.0f; // zone A side, 40m from B's edge
    if (sim.Zones().FindIndexForPosition(wake_x, y_mid) == zone_b) {
        check("wake-point-outside", false);
        sim.Stop();
        return failures + 1;
    }
    check("wake-point-outside", true);
    constexpr gs::common::SessionId kWaker = 701;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kWaker);
        sim.PostSpawn(session, MakeBenchCharacter(901), gs::game::DebugSpawnOverride{wake_x, y_mid});
    }
    const bool spawned_waker = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Owners().size() == 1;
    });
    check("waker-spawned", spawned_waker && !expired());
    const bool rewoke = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Zones().GetZone(zone_b).Activity() == gs::game::ZoneActivity::Active;
    });
    check("predictive-wake", rewoke && !expired());
    check("validate-wake", validate_now("wake"));

    const auto pm = sim.PartitionMetricsSnapshot();
    std::printf("ACTIVITY metrics: splits=%llu merges=%llu\n",
                (unsigned long long)(pm.split_commits + pm.split_aborts),
                (unsigned long long)(pm.merge_commits + pm.merge_aborts));
    sim.Stop();
    std::printf("ACTIVITY-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Continuous load field live scenario (Adaptive Simulation Fabric phase 1).
// Own sim lifecycle, 1km test map, 100m L0 cells (10x10) so hotspot
// localization is observable. Verifies that work lands where it happens, the
// channel breakdown stays separated, normalization/composite are bounded, the
// field survives a topology change without gaining load (§25), smoothing
// ramps fast and decays, and the generation audit passes. Ends with a pure
// resolution sweep (250/500/1000m over the 100km target world) measuring
// memory, active cells and aggregation cost. Returns failure count.
int RunLoadFieldScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    using gs::game::LoadChannel;
    using gs::game::LoadTimescale;

    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("LOADFIELD %s: PASS\n", name);
        } else {
            std::printf("LOADFIELD %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);

    auto validate_now = [&](const char* what) -> bool {
        sim.RequestLoadFieldValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeLoadFieldValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("LOADFIELD validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("LOADFIELD validation(%s): TIMEOUT\n", what);
        return false;
    };

    // Small LOD radii so far mobs go Dormant (zero work) on the 1km map.
    gs::game::LodConfig test_lod;
    test_lod.full_radius_m = 15.0f;
    test_lod.reduced_radius_m = 50.0f;
    test_lod.low_radius_m = 150.0f;
    test_lod.demote_full_sec = 1.0f;
    test_lod.demote_reduced_sec = 1.0f;
    test_lod.demote_low_sec = 1.0f;
    sim.ConfigureSimulationLod(test_lod);

    // Partition floor allows the 500m test zones to split (validated floor is
    // 2x AOI radius; 250 passes).
    gs::game::PartitionConfig partition;
    partition.min_zone_size_m = 250.0f;
    sim.ConfigurePartition(partition);

    // Load field: 100m cells, 4Hz aggregation for observable smoothing,
    // reference budgets scaled to the tiny test population.
    gs::game::LoadFieldConfig lf;
    lf.cell_size_m = 100.0f;
    lf.aggregation_hz = 4.0f;
    lf.l1_enabled = true;
    lf.l1_ratio = 2;
    lf.simulation_budget = 200.0f;
    lf.replication_budget = 200000.0f;
    lf.aoi_budget = 5000.0f;
    lf.combat_budget = 20.0f;
    lf.migration_budget = 10.0f;
    sim.ConfigureLoadField(lf);
    const bool config_ok = sim.EffectiveLoadFieldConfig().cell_size_m == 100.0f &&
                           sim.EffectiveLoadFieldConfig().aggregation_hz == 4.0f &&
                           sim.EffectiveLoadFieldConfig().enabled;
    check("config-authority", config_ok);
    if (!config_ok) {
        return failures + 1;
    }
    sim.Start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(120, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // ---- Phase 1: two hotspots. Player + 5 static mobs at (100,100); a
    // second player at (850,850). Everything stationary: positions are
    // bit-exact, so the spatial asserts are deterministic.
    constexpr gs::common::SessionId kPlayer = 750;
    constexpr gs::common::SessionId kFarPlayer = 751;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer);
        sim.PostSpawn(session, MakeBenchCharacter(950), gs::game::DebugSpawnOverride{100.0f, 100.0f});
    }
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kFarPlayer);
        sim.PostSpawn(session, MakeBenchCharacter(951),
                      gs::game::DebugSpawnOverride{850.0f, 850.0f});
    }
    for (int k = 0; k < 5; ++k) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = 100.0f + static_cast<float>(k) * 2.0f;
        point.y = 100.0f;
        point.count = 1;
        point.radius = 0.0f; // static mob: exact position forever
        sim.AddMobSpawnPoint(point);
    }
    for (int k = 0; k < 5; ++k) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + k));
    }
    const bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 2 && sim.CollectProcessLoad().mobs >= 5;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }

    // ---- Phase 2: work lands where it happens. Sample across the ramp so
    // the fast-leads-slow ordering is observable.
    bool saw_fast_above_slow = false;
    bool hotspot_ok = false;
    bool second_hotspot_ok = false;
    bool cold_cell_ok = false;
    bool channels_ok = false;
    bool bounds_ok = false;
    gs::game::LoadTotals peak_totals;
    float peak_sim_fast = 0.0f;
    float peak_combat_fast = 0.0f;
    // Tracks the largest per-window totals seen across the whole scenario, so
    // the report shows the combat/migration windows too (they happen after
    // the ramp phase). One call per poll; the field rebuilds at 4Hz here.
    auto track_peak_totals = [&]() {
        const auto metrics = sim.LoadFieldMetrics();
        peak_totals.sim_work = std::max(peak_totals.sim_work, metrics.last_totals.sim_work);
        peak_totals.repl_bytes = std::max(peak_totals.repl_bytes, metrics.last_totals.repl_bytes);
        peak_totals.repl_records =
            std::max(peak_totals.repl_records, metrics.last_totals.repl_records);
        peak_totals.repl_dirty = std::max(peak_totals.repl_dirty, metrics.last_totals.repl_dirty);
        peak_totals.aoi_queries =
            std::max(peak_totals.aoi_queries, metrics.last_totals.aoi_queries);
        peak_totals.aoi_candidates =
            std::max(peak_totals.aoi_candidates, metrics.last_totals.aoi_candidates);
        peak_totals.combat_events =
            std::max(peak_totals.combat_events, metrics.last_totals.combat_events);
        peak_totals.migration_events =
            std::max(peak_totals.migration_events, metrics.last_totals.migration_events);
    };
    const auto sample_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < sample_until && !expired()) {
        const auto grid = sim.LoadFieldSnapshot();
        if (grid != nullptr && grid->enabled) {
            const auto* hot = grid->CellAt(100.0f, 100.0f);
            const auto* cold = grid->CellAt(500.0f, 500.0f);
            const auto* far_cell = grid->CellAt(850.0f, 850.0f);
            if (hot != nullptr && cold != nullptr && far_cell != nullptr) {
                hotspot_ok = hot->current[LoadChannel::Simulation] > 0.0f &&
                             hot->fast[LoadChannel::Simulation] > 0.0f;
                second_hotspot_ok = far_cell->current[LoadChannel::Simulation] > 0.0f;
                cold_cell_ok = cold->current.IsZero() && cold->fast.IsZero() &&
                               cold->slow.IsZero();
                channels_ok = hot->current[LoadChannel::Replication] > 0.0f &&
                              hot->current[LoadChannel::AOI] > 0.0f;
                const auto normalized =
                    grid->NormalizedLoadAt(100.0f, 100.0f, LoadTimescale::Slow);
                bounds_ok = true;
                for (std::size_t c = 0; c < gs::game::kLoadChannelCount; ++c) {
                    if (!(normalized.values[c] >= 0.0f) || normalized.values[c] > 1.0f) {
                        bounds_ok = false;
                    }
                }
                if (!(normalized.composite >= 0.0f) || normalized.composite > 1.0f) {
                    bounds_ok = false;
                }
                if (hot->fast[LoadChannel::Simulation] >
                    hot->slow[LoadChannel::Simulation]) {
                    saw_fast_above_slow = true;
                }
                peak_sim_fast = std::max(peak_sim_fast, hot->fast[LoadChannel::Simulation]);
            }
        }
        track_peak_totals();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check("hotspot-localized", hotspot_ok);
    check("second-hotspot", second_hotspot_ok);
    check("cold-cell-zero", cold_cell_ok);
    check("channel-breakdown", channels_ok);
    check("normalized-bounds", bounds_ok);
    check("fast-leads-slow", saw_fast_above_slow);
    check("validate-ramp", validate_now("ramp"));

    // ---- Phase 3: combat heat appears at the hotspot and decays after the
    // fight stops (a 5-minute-old fight must not stay hot).
    const std::uint64_t attacks_before = sim.AttacksTotal();
    const auto attack_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < attack_until && !expired()) {
        for (std::uint32_t net = 1000000; net < 1000010; ++net) {
            sim.PostAttackTarget(kPlayer, net);
        }
        track_peak_totals();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    const bool attacked = sim.AttacksTotal() > attacks_before;
    bool combat_hot = false;
    const auto combat_wait = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < combat_wait && !combat_hot) {
        const auto grid = sim.LoadFieldSnapshot();
        if (grid != nullptr) {
            const auto* hot = grid->CellAt(100.0f, 100.0f);
            if (hot != nullptr && hot->fast[LoadChannel::Combat] > 0.0f) {
                combat_hot = true;
            }
            if (hot != nullptr) {
                peak_combat_fast = std::max(peak_combat_fast, hot->fast[LoadChannel::Combat]);
            }
        }
        track_peak_totals();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("LOADFIELD combat: attacks=%llu peak_fast=%.3f\n",
                (unsigned long long)(sim.AttacksTotal() - attacks_before),
                peak_combat_fast);
    check("combat-heat", attacked && combat_hot && !expired());

    // ---- Phase 4: §25 topology independence. Force-split the EMPTY zone 2
    // (250,750): no residents => no transfers => the field must gain no
    // migration work and must not reset while the topology changes.
    const std::size_t zone_count_before = sim.Zones().ZoneCount();
    const std::size_t empty_zone_index = sim.Zones().FindIndexForPosition(250.0f, 750.0f);
    const bool empty_zone_found = empty_zone_index < sim.Zones().ZoneCount();
    check("empty-zone-found", empty_zone_found);
    if (empty_zone_found) {
        const gs::game::ZoneId empty_zone_id = sim.Zones().GetZone(empty_zone_index).Id();
        sim.PostForceSplit(empty_zone_id);
        const bool split_done = WaitFor(std::chrono::seconds(10), [&] {
            return sim.Zones().ZoneCount() == zone_count_before + 4;
        });
        check("empty-split-committed", split_done && !expired());
        bool no_migration_work = true;
        bool hotspot_preserved = true;
        const auto observe_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < observe_until && !expired()) {
            const auto grid = sim.LoadFieldSnapshot();
            if (grid != nullptr) {
                for (const auto& cell : grid->cells) {
                    if (cell.current[LoadChannel::Migration] != 0.0f ||
                        cell.fast[LoadChannel::Migration] != 0.0f) {
                        no_migration_work = false;
                    }
                }
                const auto* hot = grid->CellAt(100.0f, 100.0f);
                if (hot == nullptr || hot->fast[LoadChannel::Simulation] <= 0.0f) {
                    hotspot_preserved = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check("topology-change-no-load", no_migration_work);
        check("field-survives-topology", hotspot_preserved);
    }

    // ---- Phase 5: split-internal transfers ARE migration work. Force-split
    // the hotspot zone; its residents move to children and must be counted.
    const std::size_t hotspot_zone_index = sim.Zones().FindIndexForPosition(100.0f, 100.0f);
    const bool hotspot_zone_found = hotspot_zone_index < sim.Zones().ZoneCount();
    check("hotspot-zone-found", hotspot_zone_found);
    if (hotspot_zone_found) {
        const gs::game::ZoneId hotspot_zone_id = sim.Zones().GetZone(hotspot_zone_index).Id();
        sim.PostForceSplit(hotspot_zone_id);
        bool migration_seen = false;
        const auto migration_wait = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < migration_wait && !migration_seen && !expired()) {
            const auto metrics = sim.LoadFieldMetrics();
            if (metrics.last_totals.migration_events > 0) {
                migration_seen = true;
            }
            const auto grid = sim.LoadFieldSnapshot();
            if (grid != nullptr) {
                for (const auto& cell : grid->cells) {
                    if (cell.fast[LoadChannel::Migration] > 0.0f) {
                        migration_seen = true;
                    }
                }
            }
            track_peak_totals();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check("split-transfers-counted", migration_seen);
    }
    check("validate-after-splits", validate_now("splits"));

    // ---- Phase 6: decay. Despawn both players (mobs go Dormant after the
    // short graces), then the hotspot load must fall well below its peak.
    sim.PostDespawn(kPlayer);
    sim.PostDespawn(kFarPlayer);
    const bool drained = WaitFor(std::chrono::seconds(10), [&] { return sim.Owners().empty(); });
    check("despawn-drained", drained && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const auto decay_start_grid = sim.LoadFieldSnapshot();
    float before_decay = 0.0f;
    if (decay_start_grid != nullptr) {
        const auto* hot = decay_start_grid->CellAt(100.0f, 100.0f);
        before_decay = hot != nullptr ? hot->fast[LoadChannel::Simulation] : 0.0f;
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    const auto decay_end_grid = sim.LoadFieldSnapshot();
    float after_decay = 0.0f;
    if (decay_end_grid != nullptr) {
        const auto* hot = decay_end_grid->CellAt(100.0f, 100.0f);
        after_decay = hot != nullptr ? hot->fast[LoadChannel::Simulation] : 0.0f;
    }
    std::printf("LOADFIELD decay: peak_fast=%.3f before=%.3f after=%.3f\n",
                peak_sim_fast,
                before_decay,
                after_decay);
    check("hotspot-decays", after_decay < before_decay * 0.6f && after_decay >= 0.0f);
    check("validate-final", validate_now("final"));

    const auto metrics = sim.LoadFieldMetrics();
    std::printf("LOADFIELD metrics: rebuilds=%llu cells=%llu active=%llu l1=%llu/%llu rb_us=%llu "
                "entries=%llu peak=[norm=%.3f comp=%.3f]\n",
                (unsigned long long)metrics.rebuilds,
                (unsigned long long)metrics.cells_total,
                (unsigned long long)metrics.cells_active,
                (unsigned long long)metrics.l1_cells_active,
                (unsigned long long)metrics.l1_cells_total,
                (unsigned long long)metrics.last_rebuild_us,
                (unsigned long long)metrics.drained_entries,
                metrics.peak_normalized,
                metrics.peak_composite);
    std::printf("LOADFIELD peak window totals: sim=%llu bytes=%llu records=%llu dirty=%llu "
                "aoi_q=%llu aoi_c=%llu combat=%llu mig=%llu\n",
                (unsigned long long)peak_totals.sim_work,
                (unsigned long long)peak_totals.repl_bytes,
                (unsigned long long)peak_totals.repl_records,
                (unsigned long long)peak_totals.repl_dirty,
                (unsigned long long)peak_totals.aoi_queries,
                (unsigned long long)peak_totals.aoi_candidates,
                (unsigned long long)peak_totals.combat_events,
                (unsigned long long)peak_totals.migration_events);

    sim.Stop();

    // ---- Resolution sweep (pure, no world). Synthetic entries stand in for
    // zone publications; the measured cost is the real aggregation cost over
    // the 100km target world. Reports memory, active cells, rebuild time and
    // hotspot contrast (dense 2km cluster vs sparse single-cell hotspots).
    for (const float cell_size : {250.0f, 500.0f, 1000.0f}) {
        gs::game::LoadFieldConfig sweep_cfg;
        sweep_cfg.cell_size_m = cell_size;
        sweep_cfg.bounds = gs::game::WorldBounds::FromExtent(100000.0f);
        sweep_cfg.aggregation_hz = 1.0f;
        sweep_cfg.l1_enabled = true;
        sweep_cfg.l1_ratio = 4;
        gs::game::ContinuousLoadField field(sweep_cfg);

        std::mt19937 sweep_rng(4242);
        std::uniform_real_distribution<float> dist(0.0f, 100000.0f);
        std::vector<gs::game::LoadBinEntry> entries;
        entries.reserve(200 + 4000);
        for (int h = 0; h < 200; ++h) {
            gs::game::LoadBinEntry entry;
            const auto mapping = gs::game::LoadFieldMapping::FromConfig(sweep_cfg);
            entry.gx = mapping.CellX(dist(sweep_rng));
            entry.gy = mapping.CellY(dist(sweep_rng));
            entry.counters.sim_work = 10;
            entries.push_back(entry);
        }
        const auto mapping = gs::game::LoadFieldMapping::FromConfig(sweep_cfg);
        for (int e = 0; e < 4000; ++e) {
            gs::game::LoadBinEntry entry;
            entry.gx = mapping.CellX(50000.0f + dist(sweep_rng) * 0.01f);
            entry.gy = mapping.CellY(50000.0f + dist(sweep_rng) * 0.01f);
            entry.counters.sim_work = 10;
            entries.push_back(entry);
        }

        double rebuild_us_total = 0.0;
        for (int r = 0; r < 10; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            field.RebuildFromEntries(entries, 1.0);
            rebuild_us_total += static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
        }
        const auto sweep_metrics = field.Metrics();
        const auto sweep_grid = field.Snapshot();
        const double memory_mb =
            static_cast<double>(sweep_grid->CellCount() + sweep_grid->L1CellCount()) *
            static_cast<double>(sizeof(gs::game::LoadCell)) / 1.0e6;
        float sparse_peak = 0.0f;
        float dense_peak = 0.0f;
        for (std::uint32_t gy = 0; gy < sweep_grid->dim_y; ++gy) {
            for (std::uint32_t gx = 0; gx < sweep_grid->dim_x; ++gx) {
                const auto& cell =
                    sweep_grid->cells[static_cast<std::size_t>(gy) * sweep_grid->dim_x + gx];
                const float value = cell.fast[LoadChannel::Simulation];
                if (value <= 0.0f) {
                    continue;
                }
                const float wx = sweep_grid->bounds.min_x +
                                 (static_cast<float>(gx) + 0.5f) * sweep_grid->cell_size_m;
                const float wy = sweep_grid->bounds.min_y +
                                 (static_cast<float>(gy) + 0.5f) * sweep_grid->cell_size_m;
                const bool in_cluster = wx > 49000.0f && wx < 51000.0f && wy > 49000.0f &&
                                        wy < 51000.0f;
                if (in_cluster) {
                    dense_peak = std::max(dense_peak, value);
                } else {
                    sparse_peak = std::max(sparse_peak, value);
                }
            }
        }
        std::printf("LOADFIELD-RES cell=%.0fm cells=%zu active=%llu l1=%llu mem_mb=%.2f "
                    "rb_avg_us=%.1f entries=%llu dense/sparse=%.1f\n",
                    cell_size,
                    sweep_grid->CellCount(),
                    (unsigned long long)sweep_metrics.cells_active,
                    (unsigned long long)sweep_metrics.l1_cells_active,
                    memory_mb,
                    rebuild_us_total / 10.0,
                    (unsigned long long)sweep_metrics.drained_entries,
                    sparse_peak > 0.0f ? dense_peak / sparse_peak : 0.0f);
    }

    std::printf("LOADFIELD-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// --- pure Adaptive Partition Scoring checks (no world, no threads) ----------
// Synthetic load grids drive the real scorer: aggregation, hotspot flood fill,
// candidate generation/determinism, balance formula, boundary/hotspot
// penalties, instability, config validation and the decision formatter.
struct ScoreTestSample {
    float composite = 0.0f;   // normalized composite (simulation channel)
    float migration = 0.0f;   // normalized migration channel
    float replication = 0.0f; // normalized replication channel
    float combat = 0.0f;      // normalized combat channel
};

gs::game::LoadGrid MakeScoreTestGrid(
    int dim,
    float cell,
    const std::function<ScoreTestSample(float, float)>& sampler)
{
    using namespace gs::game;
    LoadGrid grid;
    grid.enabled = true;
    grid.cell_size_m = cell;
    grid.bounds = WorldBounds::FromExtent(static_cast<float>(dim) * cell);
    grid.dim_x = static_cast<std::uint32_t>(dim);
    grid.dim_y = static_cast<std::uint32_t>(dim);
    grid.window_seconds = 1.0f;
    grid.config.l1_enabled = false;
    grid.cells.assign(static_cast<std::size_t>(dim) * dim, LoadCell{});
    for (int y = 0; y < dim; ++y) {
        for (int x = 0; x < dim; ++x) {
            const float cx = (static_cast<float>(x) + 0.5f) * cell;
            const float cy = (static_cast<float>(y) + 0.5f) * cell;
            const ScoreTestSample sample = sampler(cx, cy);
            auto& cell = grid.cells[static_cast<std::size_t>(y) * dim + x];
            // Channel values chosen so NormalizeLoad returns exactly the
            // sampler's normalized numbers (default budgets, window 1s).
            cell.fast[LoadChannel::Simulation] = sample.composite * 5000.0f;
            cell.fast[LoadChannel::Migration] = sample.migration * 50.0f;
            cell.fast[LoadChannel::Replication] = sample.replication * 1000000.0f;
            cell.fast[LoadChannel::Combat] = sample.combat * 100.0f;
        }
    }
    return grid;
}

int RunPartitionScoreSelftest()
{
    using namespace gs::game;
    int failures = 0;

    // (1) Rect aggregation: exact sums, peak position, centroid, empty and
    // outside rects, half-open tiling (children sum == parent).
    {
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            ScoreTestSample sample;
            sample.composite = (x < 200.0f && y < 200.0f) ? 1.0f : 0.0f;
            return sample;
        });
        const auto block = AggregateLoad(grid, WorldBounds{0, 0, 200, 200}, LoadTimescale::Fast);
        const bool block_ok =
            block.valid && block.cells == 4 && std::abs(block.composite_sum - 4.0f) < 1e-3f &&
            std::abs(block.composite_peak - 1.0f) < 1e-3f && block.active_cells == 4 &&
            std::abs(block.centroid_x - 100.0f) < 1e-3f &&
            std::abs(block.centroid_y - 100.0f) < 1e-3f;
        const auto cold = AggregateLoad(grid, WorldBounds{200, 200, 1000, 1000}, LoadTimescale::Fast);
        const bool cold_ok = cold.valid && cold.cells == 64 && cold.composite_sum == 0.0f;
        const auto outside =
            AggregateLoad(grid, WorldBounds{2000, 2000, 3000, 3000}, LoadTimescale::Fast);
        const bool outside_ok = outside.valid && outside.cells == 0;
        const WorldBounds parent_rect{0, 0, 400, 400};
        float children_sum = 0.0f;
        const WorldBounds children[4] = {{0, 0, 200, 200},
                                         {200, 0, 400, 200},
                                         {0, 200, 200, 400},
                                         {200, 200, 400, 400}};
        for (const auto& child : children) {
            children_sum += AggregateLoad(grid, child, LoadTimescale::Fast).composite_sum;
        }
        const bool tiling =
            std::abs(AggregateLoad(grid, parent_rect, LoadTimescale::Fast).composite_sum -
                     children_sum) < 1e-3f;
        SelftestReport("partitionscore-aggregate",
                       block_ok && cold_ok && outside_ok && tiling,
                       false,
                       failures);
    }

    // (2) Hotspot flood fill: connected components, deterministic order.
    {
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            const int gx = static_cast<int>(x / 100.0f);
            const int gy = static_cast<int>(y / 100.0f);
            ScoreTestSample sample;
            const bool cluster =
                (gx == 1 && gy == 1) || (gx == 2 && gy == 1) || (gx == 1 && gy == 2);
            const bool single = (gx == 8 && gy == 8);
            sample.composite = cluster ? 0.9f : (single ? 0.8f : 0.0f);
            return sample;
        });
        std::vector<std::uint8_t> scratch;
        const auto hotspots = DetectLoadHotspots(grid, WorldBounds{0, 0, 1000, 1000},
                                                 LoadTimescale::Fast, 0.5f, 4, scratch);
        const bool ok = hotspots.size() == 2 && hotspots[0].cells == 3 &&
                        std::abs(hotspots[0].load - 2.7f) < 1e-2f &&
                        std::abs(hotspots[0].bounds.min_x - 100.0f) < 1e-3f &&
                        std::abs(hotspots[0].bounds.max_x - 300.0f) < 1e-3f &&
                        hotspots[1].cells == 1;
        SelftestReport("partitionscore-hotspot-detect", ok, false, failures);
    }

    // (3) Candidate generation: deterministic list, midpoint baseline crosses
    // the hotspot, hotspot-aware candidates exist and the weighted scorer can
    // prefer a non-crossing cut.
    {
        // Hotspot straddling the geometric midpoint on X (cells x 4..6).
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            const int gx = static_cast<int>(x / 100.0f);
            const int gy = static_cast<int>(y / 100.0f);
            ScoreTestSample sample;
            sample.composite = (gx >= 4 && gx < 7 && gy >= 1 && gy < 3) ? 0.9f : 0.0f;
            return sample;
        });
        PartitionScorer scorer;
        PartitionScoringConfig config = scorer.GetConfig();
        config.min_zone_size_m = 100.0f;
        config.hotspot_threshold = 0.5f;
        config.weight_balance = 1.0f;
        // Hotspot-avoidance priority for this test: keeping the hotspot whole
        // costs all balance benefit here, so the boundary weight must exceed
        // the balance benefit to flip the decision (documented trade-off).
        config.weight_boundary = 5.0f;
        scorer.SetConfig(config);
        PartitionScoreInput input;
        input.zone_id = 1;
        input.bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        const auto now = std::chrono::steady_clock::now();
        const auto rec = scorer.ScoreSplit(input, &grid, nullptr, now);
        const auto rec2 = scorer.ScoreSplit(input, &grid, nullptr, now);
        bool deterministic = rec.valid && rec2.valid &&
                             rec.candidates.size() == rec2.candidates.size();
        if (deterministic) {
            for (std::size_t i = 0; i < rec.candidates.size(); ++i) {
                deterministic = deterministic && rec.candidates[i].kind == rec2.candidates[i].kind &&
                                rec.candidates[i].center.x == rec2.candidates[i].center.x &&
                                rec.candidates[i].center.y == rec2.candidates[i].center.y &&
                                rec.candidates[i].final_score == rec2.candidates[i].final_score;
            }
        }
        const bool midpoint_first = !rec.candidates.empty() &&
                                    rec.candidates[0].kind == SplitCandidateKind::Midpoint;
        const bool midpoint_crosses = rec.baseline.hotspots_crossed == 1;
        bool has_hotspot_candidate = false;
        for (const auto& candidate : rec.candidates) {
            if (candidate.kind == SplitCandidateKind::HotspotX ||
                candidate.kind == SplitCandidateKind::HotspotY ||
                candidate.kind == SplitCandidateKind::HotspotCorner) {
                has_hotspot_candidate = true;
            }
        }
        const bool best_avoids = rec.valid && rec.best.hotspots_crossed == 0;
        const bool best_beats = rec.best.final_score >= rec.baseline.final_score;
        std::printf("PARTITIONSCORE candidates=%zu best=%s@(%.0f,%.0f) score=%.3f "
                    "midpoint=%.3f crossed=%u/%u\n",
                    rec.candidates.size(),
                    gs::game::SplitCandidateKindName(rec.best.kind),
                    rec.best.center.x,
                    rec.best.center.y,
                    rec.best.final_score,
                    rec.baseline.final_score,
                    rec.best.hotspots_crossed,
                    rec.baseline.hotspots_crossed);
        SelftestReport("partitionscore-candidates",
                       deterministic && midpoint_first && midpoint_crosses &&
                           has_hotspot_candidate && best_avoids && best_beats,
                       false,
                       failures);
    }

    // (4) Balance formula: uniform load -> peak reduction ~0.75; 99/1 load ->
    // almost no reduction (a split must not be rewarded for being technical).
    {
        const LoadGrid uniform = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            ScoreTestSample sample;
            sample.composite = 0.1f;
            return sample;
        });
        PartitionScorer scorer;
        PartitionScoringConfig config = scorer.GetConfig();
        config.min_zone_size_m = 100.0f;
        config.weight_boundary = 0.0f; // isolate the balance term
        config.topology_penalty = 0.0f;
        scorer.SetConfig(config);
        PartitionScoreInput input;
        input.bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        const auto now = std::chrono::steady_clock::now();
        const auto uniform_rec = scorer.ScoreSplit(input, &uniform, nullptr, now);
        const bool uniform_ok = uniform_rec.valid &&
                                std::abs(uniform_rec.baseline.peak_reduction - 0.75f) < 0.05f &&
                                std::abs(uniform_rec.baseline.balance_ratio - 1.0f) < 0.05f;

        const LoadGrid skewed = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            ScoreTestSample sample;
            sample.composite = (x < 500.0f && y < 500.0f) ? 1.0f : 0.01f;
            return sample;
        });
        const auto skewed_rec = scorer.ScoreSplit(input, &skewed, nullptr, now);
        // The midpoint leaves the dominant quadrant whole: the peak barely
        // drops (~97% of the load is in one child). The scorer is free to
        // look for a better cut, so the assertion is on the baseline.
        const bool skewed_ok = skewed_rec.valid && skewed_rec.baseline.peak_reduction < 0.2f;
        std::printf("PARTITIONSCORE balance: uniform_red=%.3f ratio=%.3f skewed_mid_red=%.3f\n",
                    uniform_rec.baseline.peak_reduction,
                    uniform_rec.baseline.balance_ratio,
                    skewed_rec.baseline.peak_reduction);
        SelftestReport("partitionscore-balance", uniform_ok && skewed_ok, false, failures);
    }

    // (5) Boundary penalty: measured migration churn sitting ON a candidate
    // cut raises its penalty, and the load distribution pulls the best
    // candidate away from it.
    {
        // Load concentrated in the west 40% (so centroid/balanced cuts move
        // west); migration churn in one center cell (on the midpoint cuts).
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            ScoreTestSample sample;
            const int gx = static_cast<int>(x / 100.0f);
            const int gy = static_cast<int>(y / 100.0f);
            sample.composite = gx < 4 ? 1.0f : 0.0f;
            sample.migration = (gx == 5 && gy == 5) ? 1.0f : 0.0f;
            return sample;
        });
        PartitionScorer scorer;
        PartitionScoringConfig config = scorer.GetConfig();
        config.min_zone_size_m = 100.0f;
        config.weight_boundary = 1.0f;
        scorer.SetConfig(config);
        PartitionScoreInput input;
        input.bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        const auto now = std::chrono::steady_clock::now();
        const auto rec = scorer.ScoreSplit(input, &grid, nullptr, now);
        const bool ok = rec.valid && rec.baseline.migration_band > 0.9f &&
                        rec.best.migration_band < rec.baseline.migration_band &&
                        rec.best.boundary_penalty < rec.baseline.boundary_penalty;
        std::printf("PARTITIONSCORE boundary: midpoint_mig=%.3f best_mig=%.3f "
                    "midpoint_pen=%.3f best_pen=%.3f\n",
                    rec.baseline.migration_band,
                    rec.best.migration_band,
                    rec.baseline.boundary_penalty,
                    rec.best.boundary_penalty);
        SelftestReport("partitionscore-boundary", ok, false, failures);
    }

    // (6) Instability penalty: a recent mutation lowers the score; an old one
    // does not.
    {
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            ScoreTestSample sample;
            sample.composite = 0.2f;
            return sample;
        });
        PartitionScorer scorer;
        PartitionScoringConfig config = scorer.GetConfig();
        config.min_zone_size_m = 100.0f;
        scorer.SetConfig(config);
        PartitionScoreInput input;
        input.bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        const auto now = std::chrono::steady_clock::now();
        input.last_mutation = now - std::chrono::seconds(1);
        const auto recent = scorer.ScoreSplit(input, &grid, nullptr, now);
        input.last_mutation = now - std::chrono::hours(2);
        const auto old = scorer.ScoreSplit(input, &grid, nullptr, now);
        const bool ok = recent.valid && old.valid && recent.best.instability_penalty > 0.9f &&
                        old.best.instability_penalty == 0.0f &&
                        recent.best.final_score < old.best.final_score;
        SelftestReport("partitionscore-instability", ok, false, failures);
    }

    // (7) Gate semantics: an empty zone scores negative (NOOP); a uniform
    // loaded zone clears the default minimum improvement.
    {
        const LoadGrid empty = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            return ScoreTestSample{};
        });
        const LoadGrid loaded = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            ScoreTestSample sample;
            sample.composite = 0.2f;
            return sample;
        });
        PartitionScorer scorer;
        PartitionScoringConfig config = scorer.GetConfig();
        config.min_zone_size_m = 100.0f;
        scorer.SetConfig(config);
        PartitionScoreInput input;
        input.bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        const auto now = std::chrono::steady_clock::now();
        const auto empty_rec = scorer.ScoreSplit(input, &empty, nullptr, now);
        const auto loaded_rec = scorer.ScoreSplit(input, &loaded, nullptr, now);
        const bool ok = empty_rec.valid && empty_rec.best.balance_benefit == 0.0f &&
                        empty_rec.best.final_score < 0.0f &&
                        loaded_rec.best.final_score >= config.min_expected_improvement;
        SelftestReport("partitionscore-gate", ok, false, failures);
    }

    // (8) Config validation: invalid fields repaired, warnings emitted.
    {
        PartitionScoringConfig bad;
        bad.min_zone_size_m = 0.0f;
        bad.min_expected_improvement = 5.0f;
        bad.boundary_band_m = -1.0f;
        bad.hotspot_threshold = std::numeric_limits<float>::quiet_NaN();
        bad.hotspot_max_count = 99;
        bad.weight_boundary = -3.0f;
        bad.activity_band_budget = 0.0f;
        const auto validated = ValidatePartitionScoringConfig(bad);
        const bool repaired = !validated.warnings.empty() &&
                              validated.effective.min_zone_size_m == 500.0f &&
                              validated.effective.min_expected_improvement == 0.10f &&
                              validated.effective.boundary_band_m == 180.0f &&
                              validated.effective.hotspot_threshold == 0.5f &&
                              validated.effective.hotspot_max_count == 4 &&
                              validated.effective.weight_boundary == 1.0f &&
                              validated.effective.activity_band_budget == 50.0f;
        std::printf("PARTITIONSCORE config warnings=%zu\n", validated.warnings.size());
        SelftestReport("partitionscore-config-validation", repaired, false, failures);
    }

    // (9) Decision formatter: both outcomes render with the breakdown.
    {
        PartitionDecisionRecord record;
        record.zone_id = 42;
        record.executed = true;
        record.candidate.kind = SplitCandidateKind::BalancedX;
        record.candidate.center = SplitCenter{500.0f, 250.0f};
        record.candidate.final_score = 0.31f;
        record.candidate.balance_benefit = 0.55f;
        const std::string split_text = FormatPartitionDecision(record);
        record.executed = false;
        record.noop_reason = PartitionNoopReason::BelowMinImprovement;
        record.detail = "not-sustained";
        const std::string noop_text = FormatPartitionDecision(record);
        const bool ok = split_text.find("SPLIT zone=42") != std::string::npos &&
                        split_text.find("balanced-x") != std::string::npos &&
                        noop_text.find("NOOP zone=42") != std::string::npos &&
                        noop_text.find("below-min-improvement") != std::string::npos &&
                        noop_text.find("not-sustained") != std::string::npos;
        SelftestReport("partitionscore-decision-format", ok, false, failures);
    }

    // (10) Merge scoring: the predicted parent load is the WHOLE-area
    // aggregate; one hot child makes the merge unsafe; a calm group with no
    // boundary churn does NOT clear the gate; measured churn on the internal
    // cuts makes it genuinely beneficial.
    {
        PartitionScorer scorer;
        const PartitionScoringConfig config = scorer.GetConfig();
        MergeScoreInput input;
        input.parent_id = 10;
        input.parent_bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        mx::map::Rect children[4];
        BuildQuadtreeChildBounds(input.parent_bounds, 500.0f, 500.0f, children);
        for (int i = 0; i < 4; ++i) {
            input.child_bounds[static_cast<std::size_t>(i)] = children[i];
            input.child_ids[static_cast<std::size_t>(i)] = static_cast<ZoneId>(11 + i);
        }
        const auto now = std::chrono::steady_clock::now();

        const LoadGrid hot_child = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            ScoreTestSample sample;
            sample.composite = (x < 500.0f && y >= 500.0f) ? 0.9f : 0.0f; // NW quadrant
            return sample;
        });
        const auto hot_rec = scorer.ScoreMerge(input, &hot_child, nullptr, now);
        const bool hot_unsafe = hot_rec.valid && !hot_rec.best.safety_ok &&
                                hot_rec.best.predicted_parent_peak >= 0.9f - 1e-3f &&
                                hot_rec.best.predicted_parent_mean < 0.3f; // whole-area mean

        const LoadGrid calm = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            ScoreTestSample sample;
            sample.composite = 0.1f;
            return sample;
        });
        const auto calm_rec = scorer.ScoreMerge(input, &calm, nullptr, now);
        const bool calm_below_gate = calm_rec.valid && calm_rec.best.safety_ok &&
                                     calm_rec.best.final_score < config.min_merge_improvement;

        const LoadGrid churn = MakeScoreTestGrid(10, 100.0f, [](float x, float y) {
            ScoreTestSample sample;
            sample.composite = 0.1f;
            const int gx = static_cast<int>(x / 100.0f);
            const int gy = static_cast<int>(y / 100.0f);
            // Migration churn along the internal cuts (x=500, y=500).
            sample.migration = (gx == 4 || gy == 4) ? 0.3f : 0.0f;
            return sample;
        });
        const auto churn_rec = scorer.ScoreMerge(input, &churn, nullptr, now);
        const bool churn_passes = churn_rec.valid && churn_rec.best.safety_ok &&
                                  churn_rec.best.final_score >= config.min_merge_improvement &&
                                  churn_rec.best.migration_benefit > 0.5f;
        std::printf("PARTITIONSCORE merge: hot=[safe=%d peak=%.2f mean=%.2f] calm=[score=%.3f] "
                    "churn=[score=%.3f mig=%.2f]\n",
                    hot_rec.best.safety_ok ? 1 : 0,
                    hot_rec.best.predicted_parent_peak,
                    hot_rec.best.predicted_parent_mean,
                    calm_rec.best.final_score,
                    churn_rec.best.final_score,
                    churn_rec.best.migration_benefit);
        SelftestReport("partitionscore-merge-scoring",
                       hot_unsafe && calm_below_gate && churn_passes,
                       false,
                       failures);
    }

    // (11) Merge determinism: identical inputs -> bit-identical output.
    {
        PartitionScorer scorer;
        const LoadGrid grid = MakeScoreTestGrid(10, 100.0f, [](float, float) {
            ScoreTestSample sample;
            sample.composite = 0.1f;
            sample.migration = 0.2f;
            return sample;
        });
        MergeScoreInput input;
        input.parent_bounds = mx::map::Rect{0.0f, 0.0f, 1000.0f, 1000.0f};
        mx::map::Rect children[4];
        BuildQuadtreeChildBounds(input.parent_bounds, 500.0f, 500.0f, children);
        for (int i = 0; i < 4; ++i) {
            input.child_bounds[static_cast<std::size_t>(i)] = children[i];
        }
        const auto now = std::chrono::steady_clock::now();
        const auto first = scorer.ScoreMerge(input, &grid, nullptr, now);
        const auto second = scorer.ScoreMerge(input, &grid, nullptr, now);
        const bool ok = first.valid && second.valid &&
                        first.best.final_score == second.best.final_score &&
                        first.best.predicted_parent_load == second.best.predicted_parent_load &&
                        first.best.boundary_benefit == second.best.boundary_benefit &&
                        first.best.migration_benefit == second.best.migration_benefit;
        SelftestReport("partitionscore-merge-determinism", ok, false, failures);
    }

    // (12) Merge/stability config validation: every invalid field repaired,
    // including the threshold invariant (margin < split threshold).
    {
        PartitionScoringConfig bad;
        bad.merge_sustained_low_s = -1.0f;
        bad.split_to_merge_cooldown_s = std::numeric_limits<float>::quiet_NaN();
        bad.merge_to_split_cooldown_s = -5.0f;
        bad.post_merge_safety_margin = 2.0f; // >= split threshold
        bad.min_merge_improvement = 5.0f;
        bad.weight_merge_risk = -1.0f;
        bad.oscillation_window_s = -3.0f;
        bad.emergency_p99_multiplier = 0.0f;
        const auto validated = ValidatePartitionScoringConfig(bad);
        const bool repaired =
            !validated.warnings.empty() && validated.effective.merge_sustained_low_s == 90.0f &&
            validated.effective.split_to_merge_cooldown_s == 120.0f &&
            validated.effective.merge_to_split_cooldown_s == 90.0f &&
            validated.effective.post_merge_safety_margin <
                validated.effective.split_load_threshold &&
            validated.effective.min_merge_improvement == 0.10f &&
            validated.effective.weight_merge_risk == 1.0f &&
            validated.effective.oscillation_window_s == 300.0f &&
            validated.effective.emergency_p99_multiplier == 2.0f;
        std::printf("PARTITIONSCORE merge-config warnings=%zu\n", validated.warnings.size());
        SelftestReport("partitionscore-merge-config-validation", repaired, false, failures);
    }

    // (13) Merge decision formatter: executed and why-not records.
    {
        PartitionDecisionRecord record;
        record.kind = PartitionDecisionKind::Merge;
        record.zone_id = 77;
        record.scored = true;
        record.executed = true;
        record.merge_candidate.parent_id = 77;
        record.merge_candidate.child_ids = {11, 12, 13, 14};
        record.merge_candidate.final_score = 0.22f;
        record.merge_candidate.predicted_parent_load = 0.18f;
        record.merge_candidate.safety_ok = true;
        const std::string merge_text = FormatPartitionDecision(record);
        record.executed = false;
        record.noop_reason = PartitionNoopReason::MergeRecentSplit;
        record.detail = "split-cooldown";
        const std::string noop_text = FormatPartitionDecision(record);
        const bool ok = merge_text.find("MERGE parent=77") != std::string::npos &&
                        merge_text.find("predicted=0.18") != std::string::npos &&
                        noop_text.find("MERGE-NOOP parent=77") != std::string::npos &&
                        noop_text.find("merge-recent-split") != std::string::npos &&
                        noop_text.find("split-cooldown") != std::string::npos;
        SelftestReport("partitionscore-merge-decision-format", ok, false, failures);
    }

    std::printf("PARTITIONSCORE-SELFTEST-DONE failures=%d\n", failures);
    return failures;
}

// Adaptive Partition Scoring live scenario (phase 2). Own sim lifecycle.
//
//   Phase A: a dense cluster confined within the min-zone-size floor of the
//     zone's edges. The zone is overloaded, but NO valid quadtree cut can
//     reduce its peak load -> the scorer must recommend NOOP, the production
//     loop must record a below-min-improvement decision and must NOT split.
//   Phase B: a second cluster on the far side makes a split genuinely
//     beneficial -> the scored candidate passes the gate and the EXISTING
//     transactional executor commits it; the peak child load drops.
//   Phase C: after everyone despawns and the field decays, the quiet world
//     must not split again (gate + cooldown).
// Returns failure count (0 = PASS).
int RunPartitionScoreScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("PARTITIONSCORE %s: PASS\n", name);
        } else {
            std::printf("PARTITIONSCORE %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("PARTITIONSCORE validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("PARTITIONSCORE validation(%s): TIMEOUT\n", what);
        return false;
    };

    // Load field: 100m cells on the 1km test map, 2Hz aggregation. Budgets
    // are chosen so the scenario's dense clusters saturate their cells
    // (peak ~1.0) while the map's base mobs (far away, LOD-Low) stay well
    // below the split threshold -- the scenario must only exercise zone 3.
    gs::game::LoadFieldConfig lf;
    lf.cell_size_m = 100.0f;
    lf.aggregation_hz = 2.0f;
    lf.l1_enabled = false;
    lf.simulation_budget = 1000.0f;
    lf.replication_budget = 200000.0f;
    lf.aoi_budget = 5000.0f;
    lf.combat_budget = 20.0f;
    lf.migration_budget = 10.0f;
    sim.ConfigureLoadField(lf);

    // Control plane: high enough threshold that only the scenario clusters
    // overload; short sustained window so the production loop acts inside the
    // scenario. Merges are effectively disabled for the run (threshold 0 is
    // unreachable, cooldown huge) so a decay-time merge cannot reshuffle the
    // topology under the assertions. The geometric floor is clamped to 2x AOI
    // radius (240m) by validation, which is exactly what Phase A relies on.
    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f;
    pcfg.split_load_threshold = 0.5f;
    pcfg.merge_load_threshold = 0.0f;
    pcfg.sustained_window_seconds = 2;
    pcfg.split_cooldown_seconds = 5;
    pcfg.merge_cooldown_seconds = 3600;
    pcfg.scoring.min_expected_improvement = 0.10f;
    pcfg.scoring.boundary_band_m = 60.0f;
    pcfg.scoring.hotspot_threshold = 0.4f;
    pcfg.scoring.instability_window_s = 60.0f;
    pcfg.scoring.why_not_log_seconds = 2.0f;
    sim.ConfigurePartition(pcfg);
    check("config-effective",
          sim.EffectivePartitionConfig().min_zone_size_m == 240.0f &&
              sim.EffectivePartitionScoringConfig().min_expected_improvement == 0.10f);
    sim.Start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(180, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // Zone 3 is the 500x1000 East Field (500,0)-(1000,1000): its Y cut has
    // real freedom while the X cut stays clamped near the middle.
    const std::size_t zone3_index = sim.Zones().FindIndexForPosition(750.0f, 500.0f);
    check("zone-found", zone3_index < sim.Zones().ZoneCount());
    if (zone3_index >= sim.Zones().ZoneCount()) {
        sim.Stop();
        return failures + 1;
    }
    const gs::game::ZoneId zone3_id = sim.Zones().GetZone(zone3_index).Id();

    std::size_t next_spawn_point = 4; // after the 4 base points from mob_spawns.conf
    auto spawn_cluster = [&](gs::common::SessionId base_session,
                             int character_base,
                             int players,
                             int mobs,
                             float x,
                             float y) {
        for (int i = 0; i < players; ++i) {
            boost::asio::ip::tcp::socket socket(io);
            auto session = std::make_shared<gs::network::Session>(
                std::move(socket), static_cast<gs::common::SessionId>(base_session + i));
            sim.PostSpawn(session,
                          MakeBenchCharacter(character_base + i),
                          gs::game::DebugSpawnOverride{x + static_cast<float>(i % 4) * 2.0f,
                                                       y + static_cast<float>(i / 4) * 2.0f});
        }
        for (int i = 0; i < mobs; ++i) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            point.x = x + static_cast<float>((i * 7) % 50) - 25.0f;
            point.y = y + static_cast<float>((i * 11) % 50) - 25.0f;
            point.count = 1;
            point.radius = 0.0f;
            sim.AddMobSpawnPoint(point);
            sim.RequestMobSpawn(next_spawn_point++);
        }
    };

    // ---- Phase A: cluster within 240m of the zone's west/south edges.
    spawn_cluster(800, 1000, 12, 60, 650.0f, 150.0f);
    const bool populated_a = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 12 && sim.CollectProcessLoad().mobs >= 60;
    });
    check("phase-a-populate", populated_a && !expired());
    if (!populated_a) {
        sim.Stop();
        return failures + 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(4)); // field ramp
    const auto rec_a = sim.ScorePartition(zone3_id);
    check("phase-a-scored", rec_a.valid && rec_a.zone_total_load > 0.0f);
    const float min_improvement = sim.EffectivePartitionScoringConfig().min_expected_improvement;
    check("phase-a-noop-recommended", rec_a.valid && rec_a.best.final_score < min_improvement);
    std::printf("PARTITIONSCORE phase-a: total=%.2f best=%s@(%.0f,%.0f) score=%.3f "
                "benefit=%.3f crossed=%u\n",
                rec_a.zone_total_load,
                gs::game::SplitCandidateKindName(rec_a.best.kind),
                rec_a.best.center.x,
                rec_a.best.center.y,
                rec_a.best.final_score,
                rec_a.best.balance_benefit,
                rec_a.best.hotspots_crossed);

    // Determinism holds for the SAME immutable generations (field + activity)
    // and the same zone input; retry until two consecutive reads share both
    // epochs. The world is quiet in Phase A (NOOP), so the input is stable.
    bool deterministic = false;
    for (int attempt = 0; attempt < 40 && !deterministic; ++attempt) {
        const auto first = sim.ScorePartition(zone3_id);
        const auto second = sim.ScorePartition(zone3_id);
        if (first.field_epoch != second.field_epoch ||
            first.activity_epoch != second.activity_epoch) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        deterministic = first.valid && second.valid && first.best.kind == second.best.kind &&
                        first.best.center.x == second.best.center.x &&
                        first.best.center.y == second.best.center.y &&
                        first.best.final_score == second.best.final_score;
    }
    check("phase-a-deterministic", deterministic);

    bool noop_recorded = false;
    const auto noop_until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < noop_until && !noop_recorded && !expired()) {
        for (const auto& record : sim.PartitionDecisionLog()) {
            if (record.zone_id == zone3_id && !record.executed &&
                record.noop_reason == gs::game::PartitionNoopReason::BelowMinImprovement) {
                noop_recorded = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check("phase-a-noop-logged", noop_recorded);
    std::printf("PARTITIONSCORE phase-a commits=%llu records=%zu\n",
                (unsigned long long)sim.PartitionMetricsSnapshot().split_commits,
                sim.PartitionDecisionLog().size());
    for (const auto& record : sim.PartitionDecisionLog()) {
        std::printf("PARTITIONSCORE record: %s\n", gs::game::FormatPartitionDecision(record).c_str());
    }
    check("phase-a-no-split", sim.PartitionMetricsSnapshot().split_commits == 0);

    // ---- Phase B: a second cluster on the far side. Now a cut separates the
    // load, the gate passes, and the transactional executor commits.
    spawn_cluster(900, 1100, 12, 60, 850.0f, 800.0f);
    const bool populated_b = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 24 && sim.CollectProcessLoad().mobs >= 120;
    });
    check("phase-b-populate", populated_b && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(5)); // field ramp
    const auto rec_b = sim.ScorePartition(zone3_id);
    check("phase-b-scored", rec_b.valid && rec_b.zone_total_load > rec_a.zone_total_load);
    check("phase-b-gate-pass", rec_b.valid && rec_b.best.final_score >= min_improvement);
    std::printf("PARTITIONSCORE phase-b: total=%.2f best=%s@(%.0f,%.0f) score=%.3f "
                "benefit=%.3f boundary=%.3f bands=[act=%.2f mig=%.2f cbt=%.2f hot=%.2f] "
                "mig=%.3f repl=%.3f\n",
                rec_b.zone_total_load,
                gs::game::SplitCandidateKindName(rec_b.best.kind),
                rec_b.best.center.x,
                rec_b.best.center.y,
                rec_b.best.final_score,
                rec_b.best.balance_benefit,
                rec_b.best.boundary_penalty,
                rec_b.best.activity_band,
                rec_b.best.migration_band,
                rec_b.best.combat_band,
                rec_b.best.hotspot_crossed_frac,
                rec_b.best.migration_penalty,
                rec_b.best.replication_penalty);

    const float before_total = rec_b.zone_total_load;
    // Wait for the EXECUTED record of THIS zone: a split of any other zone
    // must not satisfy the phase.
    bool executed_recorded = false;
    const bool split_committed = WaitFor(std::chrono::seconds(40), [&] {
        for (const auto& record : sim.PartitionDecisionLog()) {
            if (record.zone_id == zone3_id && record.executed && record.scored) {
                executed_recorded = true;
                return true;
            }
        }
        return false;
    });
    check("phase-b-split-committed", split_committed && !expired());
    if (executed_recorded) {
        for (const auto& record : sim.PartitionDecisionLog()) {
            if (record.zone_id == zone3_id && record.executed && record.scored) {
                std::printf("PARTITIONSCORE decision: %s\n",
                            gs::game::FormatPartitionDecision(record).c_str());
            }
        }
    }
    check("phase-b-executed-logged", executed_recorded);
    check("validate-after-split", validate_now("split"));

    // Peak reduction: after one more field generation, the worst child load
    // must be clearly below the pre-split zone total.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    float after_peak = 0.0f;
    {
        const auto field = sim.LoadFieldSnapshot();
        for (const auto* leaf : sim.Zones().GetActiveLeaves()) {
            const std::size_t index = sim.Zones().FindIndexById(leaf->zone_id);
            if (index >= sim.Zones().ZoneCount()) {
                continue;
            }
            const auto& bounds = sim.Zones().GetZone(index).Bounds();
            const auto aggregate = gs::game::AggregateLoad(
                *field,
                gs::game::WorldBounds{bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y},
                gs::game::LoadTimescale::Fast);
            if (aggregate.composite_sum > 0.0f) {
                std::printf("PARTITIONSCORE child zone=%u bounds=(%.0f,%.0f)-(%.0f,%.0f) load=%.2f\n",
                            leaf->zone_id,
                            bounds.min_x,
                            bounds.min_y,
                            bounds.max_x,
                            bounds.max_y,
                            aggregate.composite_sum);
            }
            after_peak = std::max(after_peak, aggregate.composite_sum);
        }
    }
    std::printf("PARTITIONSCORE peak: before=%.2f after_peak=%.2f\n",
                before_total,
                after_peak);
    check("phase-b-peak-reduced",
          before_total > 0.0f && after_peak < before_total * 0.8f);

    // Cooldown: no immediate second split.
    const std::uint64_t commits_after_split = sim.PartitionMetricsSnapshot().split_commits;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    check("phase-b-cooldown",
          sim.PartitionMetricsSnapshot().split_commits == commits_after_split);

    // ---- Phase C: everyone leaves; wait for the field to actually decay
    // below the overload threshold, then the quiet world must not split.
    for (gs::common::SessionId session = 800; session < 824; ++session) {
        sim.PostDespawn(session);
    }
    for (gs::common::SessionId session = 900; session < 924; ++session) {
        sim.PostDespawn(session);
    }
    const bool drained = WaitFor(std::chrono::seconds(20), [&] { return sim.Owners().empty(); });
    check("phase-c-drained", drained && !expired());
    // Decay can legitimately trigger further splits while the smoothed field
    // is still hot; wait until it is genuinely cold before asserting quiet.
    const bool cold = WaitFor(std::chrono::seconds(60), [&] {
        const auto field = sim.LoadFieldSnapshot();
        return field != nullptr && field->peak_composite < 0.1f;
    });
    check("phase-c-field-cold", cold && !expired());
    const std::uint64_t commits_before_quiet = sim.PartitionMetricsSnapshot().split_commits;
    std::this_thread::sleep_for(std::chrono::seconds(8));
    check("phase-c-quiet-no-split",
          sim.PartitionMetricsSnapshot().split_commits == commits_before_quiet);
    check("validate-final", validate_now("final"));

    const auto metrics = sim.PartitionMetricsSnapshot();
    std::printf("PARTITIONSCORE metrics: split_commits=%llu split_aborts=%llu merge_commits=%llu "
                "decisions=%zu\n",
                (unsigned long long)metrics.split_commits,
                (unsigned long long)metrics.split_aborts,
                (unsigned long long)metrics.merge_commits,
                sim.PartitionDecisionLog().size());
    sim.Stop();
    std::printf("PARTITIONSCORE-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Phase-3 partition stability scenario: sustained-low merge, directional
// cooldowns and moving-hotspot thrash protection. Own sim lifecycle on the
// 1km test map; smoothing time constants and cooldowns are compressed so the
// scenario runs in minutes while the conservative structure (two-timescale
// eligibility, cooldowns, safety margin) stays identical.
//
//   Phase A: hotspot -> scored split commits (quadtree group created).
//   Phase B: hotspot leaves; players sit on the internal cut lines so the
//     merge has a real boundary benefit -> sustained-low matures -> the
//     transactional executor commits the merge.
//   Phase C: a hotspot re-appears immediately after the merge -> the
//     merge-to-split cooldown suppresses the split (why-not "merge-cooldown";
//     emergency bypass disabled here) -> after the cooldown it commits.
//   Phase D: the hotspot moves to another child of the same group -> the
//     group is NOT eligible while one child is hot (no merge); after it
//     cools, the tree simplifies again. The oscillation counter (window
//     matched to the test cooldowns) stays zero.
int RunStabilityScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("STABILITY %s: PASS\n", name);
        } else {
            std::printf("STABILITY %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("STABILITY validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("STABILITY validation(%s): TIMEOUT\n", what);
        return false;
    };

    gs::game::LoadFieldConfig lf;
    lf.cell_size_m = 100.0f;
    lf.aggregation_hz = 4.0f;
    lf.l1_enabled = false;
    lf.simulation_budget = 1000.0f;
    lf.replication_budget = 200000.0f;
    lf.aoi_budget = 5000.0f;
    lf.combat_budget = 20.0f;
    lf.migration_budget = 10.0f;
    // Compressed smoothing: the test exercises the conservative two-timescale
    // merge eligibility without waiting minutes for the slow EMA to release.
    lf.fast_rise_tau_s = 0.5f;
    lf.fast_fall_tau_s = 2.0f;
    lf.slow_rise_tau_s = 2.0f;
    lf.slow_fall_tau_s = 4.0f;
    sim.ConfigureLoadField(lf);

    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f; // validated floor clamps to 240
    pcfg.split_load_threshold = 0.5f;
    pcfg.merge_load_threshold = 0.25f; // value hysteresis (far below split)
    pcfg.sustained_window_seconds = 2;
    pcfg.split_cooldown_seconds = 2;
    pcfg.merge_cooldown_seconds = 2;
    pcfg.scoring.merge_sustained_low_s = 3.0f; // time hysteresis (short test)
    pcfg.scoring.split_to_merge_cooldown_s = 4.0f;
    pcfg.scoring.merge_to_split_cooldown_s = 12.0f;
    pcfg.scoring.post_merge_safety_margin = 0.15f; // ceiling 0.35
    pcfg.scoring.min_merge_improvement = 0.05f;
    pcfg.scoring.merge_topology_benefit = 0.15f;
    pcfg.scoring.min_expected_improvement = 0.05f;
    pcfg.scoring.boundary_band_m = 120.0f;
    pcfg.scoring.hotspot_threshold = 0.4f;
    pcfg.scoring.oscillation_window_s = 4.0f;
    pcfg.scoring.instability_window_s = 20.0f; // matched to the compressed test timeline
    pcfg.scoring.emergency_split_bypass = false; // deterministic suppression
    pcfg.scoring.why_not_log_seconds = 2.0f;
    sim.ConfigurePartition(pcfg);
    check("config-effective",
          sim.EffectivePartitionConfig().min_zone_size_m == 240.0f &&
              sim.EffectivePartitionScoringConfig().min_merge_improvement == 0.05f &&
              !sim.EffectivePartitionScoringConfig().emergency_split_bypass);
    sim.Start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(240, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    const std::size_t zone3_index = sim.Zones().FindIndexForPosition(750.0f, 500.0f);
    check("zone-found", zone3_index < sim.Zones().ZoneCount());
    if (zone3_index >= sim.Zones().ZoneCount()) {
        sim.Stop();
        return failures + 1;
    }
    const gs::game::ZoneId zone3_id = sim.Zones().GetZone(zone3_index).Id();
    const mx::map::Rect zone3_bounds = sim.Zones().GetZone(zone3_index).Bounds();

    std::size_t next_spawn_point = 4;
    auto spawn_cluster = [&](gs::common::SessionId base_session,
                             int character_base,
                             int players,
                             int mobs,
                             float x,
                             float y) {
        for (int i = 0; i < players; ++i) {
            boost::asio::ip::tcp::socket socket(io);
            auto session = std::make_shared<gs::network::Session>(
                std::move(socket), static_cast<gs::common::SessionId>(base_session + i));
            sim.PostSpawn(session,
                          MakeBenchCharacter(character_base + i),
                          gs::game::DebugSpawnOverride{x + static_cast<float>(i % 4) * 2.0f,
                                                       y + static_cast<float>(i / 4) * 2.0f});
        }
        for (int i = 0; i < mobs; ++i) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            point.x = x + static_cast<float>((i * 7) % 50) - 25.0f;
            point.y = y + static_cast<float>((i * 11) % 50) - 25.0f;
            point.count = 1;
            point.radius = 0.0f;
            sim.AddMobSpawnPoint(point);
            sim.RequestMobSpawn(next_spawn_point++);
        }
    };
    auto despawn_range = [&](gs::common::SessionId base_session, int count) {
        for (int i = 0; i < count; ++i) {
            sim.PostDespawn(static_cast<gs::common::SessionId>(base_session + i));
        }
    };
    auto group_children = [&](gs::game::ZoneId parent_id) {
        std::vector<const gs::game::ZonePartition*> children;
        for (const auto* leaf : sim.Zones().GetActiveLeaves()) {
            if (leaf->parent != nullptr && leaf->parent->zone_id == parent_id) {
                children.push_back(leaf);
            }
        }
        return children;
    };
    auto find_leaf_with_bounds = [&](const mx::map::Rect& bounds) -> gs::game::ZoneId {
        for (const auto* leaf : sim.Zones().GetActiveLeaves()) {
            if (leaf->bounds.min_x == bounds.min_x && leaf->bounds.min_y == bounds.min_y &&
                leaf->bounds.max_x == bounds.max_x && leaf->bounds.max_y == bounds.max_y) {
                return leaf->zone_id;
            }
        }
        return 0;
    };

    // ---- Phase A: two player-only clusters make a split genuinely
    // beneficial (one cluster alone would be a NOOP: the cut cannot reduce
    // its peak). Players despawn cleanly, so no mob residue can keep the
    // group warm in Phase B.
    spawn_cluster(600, 2000, 16, 0, 650.0f, 150.0f);
    spawn_cluster(616, 2020, 16, 0, 850.0f, 800.0f);
    const bool populated_a = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 32;
    });
    check("phase-a-populate", populated_a && !expired());
    if (!populated_a) {
        sim.Stop();
        return failures + 1;
    }
    const bool split_a = WaitFor(std::chrono::seconds(40), [&] {
        return sim.PartitionMetricsSnapshot().split_commits >= 1;
    });
    check("phase-a-split", split_a && !expired());
    const auto children_a = group_children(zone3_id);
    check("phase-a-four-children", children_a.size() == 4);
    if (children_a.size() != 4) {
        sim.Stop();
        return failures + 1;
    }

    // ---- Phase B: hotspot leaves; players on the internal cuts give the
    // merge a real, measurable boundary benefit. Positions avoid the test
    // map's blocked square (x 720..822, y 240..342) and wall (y 490..496 at
    // x 360..642): X-cut players sit at y=600, Y-cut players at x=660.
    despawn_range(600, 32);
    const bool drained_a = WaitFor(std::chrono::seconds(15), [&] { return sim.Owners().empty(); });
    check("phase-b-drained", drained_a && !expired());
    const float cut_x = children_a[0]->bounds.max_x; // NW east edge
    const float cut_y = children_a[0]->bounds.min_y; // NW south edge
    const float boundary_positions[4][2] = {{cut_x - 20.0f, 600.0f},
                                            {cut_x + 20.0f, 600.0f},
                                            {660.0f, cut_y - 20.0f},
                                            {660.0f, cut_y + 20.0f}};
    for (int i = 0; i < 4; ++i) {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(
            std::move(socket), static_cast<gs::common::SessionId>(640 + i));
        sim.PostSpawn(session,
                      MakeBenchCharacter(2100 + i),
                      gs::game::DebugSpawnOverride{boundary_positions[i][0],
                                                   boundary_positions[i][1]});
    }
    const bool populated_b = WaitFor(std::chrono::seconds(15), [&] {
        return sim.Owners().size() == 4;
    });
    check("phase-b-populate", populated_b && !expired());
    // The group must be genuinely low on BOTH field timescales (conservative
    // eligibility) before the merge can be scored. Wait until the residual
    // boundary load has settled well below the ceiling so the risk term is
    // small -- the same conservatism the production defaults enforce.
    const bool group_low = WaitFor(std::chrono::seconds(90), [&] {
        const auto rec = sim.ScoreMerge(zone3_id);
        return rec.valid && rec.best.safety_ok && rec.best.predicted_parent_load < 0.15f;
    });
    check("phase-b-group-low", group_low && !expired());
    const auto merge_rec = sim.ScoreMerge(zone3_id);
    check("phase-b-merge-scored", merge_rec.valid && merge_rec.best.activity_band > 0.0f);
    // The gate passes once the residual boundary load has settled and the
    // split's instability penalty has decayed -- exactly what the production
    // controller waits for.
    const bool gate_ready = WaitFor(std::chrono::seconds(60), [&] {
        const auto rec = sim.ScoreMerge(zone3_id);
        return rec.valid && rec.best.safety_ok &&
               rec.best.final_score >=
                   sim.EffectivePartitionScoringConfig().min_merge_improvement;
    });
    check("phase-b-merge-gate", gate_ready && !expired());
    const auto gate_rec = sim.ScoreMerge(zone3_id);
    std::printf("STABILITY phase-b merge: score=%.3f predicted=%.3f ceiling=%.2f "
                "topology=%.2f boundary=%.2f activity=%.2f migration=%.2f exec=%.3f risk=%.3f "
                "instability=%.3f\n",
                gate_rec.best.final_score,
                gate_rec.best.predicted_parent_load,
                gate_rec.best.safety_ceiling,
                gate_rec.best.topology_benefit,
                gate_rec.best.boundary_benefit,
                gate_rec.best.activity_band,
                gate_rec.best.migration_benefit,
                gate_rec.best.execution_penalty,
                gate_rec.best.parent_load_risk,
                gate_rec.best.instability_penalty);
    const bool merged = WaitFor(std::chrono::seconds(60), [&] {
        return sim.PartitionMetricsSnapshot().merge_commits >= 1;
    });
    check("phase-b-merge-committed", merged && !expired());
    bool merge_logged = false;
    for (const auto& record : sim.PartitionDecisionLog()) {
        if (record.kind == gs::game::PartitionDecisionKind::Merge && record.executed &&
            record.zone_id == zone3_id) {
            merge_logged = true;
            std::printf("STABILITY decision: %s\n",
                        gs::game::FormatPartitionDecision(record).c_str());
        }
    }
    check("phase-b-merge-logged", merge_logged);
    check("validate-after-merge", validate_now("merge"));
    const gs::game::ZoneId merged_zone_id = find_leaf_with_bounds(zone3_bounds);
    check("phase-b-merged-leaf", merged_zone_id != 0);

    // ---- Phase C: hotspot right after the merge -> merge-to-split cooldown
    // suppresses the split; after it expires the split commits. Two clusters
    // again so the eventual split is genuinely beneficial.
    spawn_cluster(660, 2200, 16, 0, 600.0f, 150.0f);
    spawn_cluster(676, 2220, 16, 0, 875.0f, 800.0f);
    const bool populated_c = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 36;
    });
    check("phase-c-populate", populated_c && !expired());
    const bool suppressed = WaitFor(std::chrono::seconds(8), [&] {
        return sim.PartitionMetricsSnapshot().split_suppressed_merge_cooldown > 0;
    });
    check("phase-c-split-suppressed", suppressed && !expired());
    bool cooldown_logged = false;
    for (const auto& record : sim.PartitionDecisionLog()) {
        if (record.kind == gs::game::PartitionDecisionKind::Split && !record.executed &&
            std::string(record.detail) == "merge-cooldown") {
            cooldown_logged = true;
        }
    }
    check("phase-c-cooldown-logged", cooldown_logged);
    const bool split_c = WaitFor(std::chrono::seconds(40), [&] {
        return sim.PartitionMetricsSnapshot().split_commits >= 2;
    });
    check("phase-c-split-after-cooldown", split_c && !expired());

    // ---- Phase D: the hotspot moves to another child of the same group.
    // While one child is hot the group is not merge eligible; after it cools,
    // the tree simplifies again. No thrash.
    const auto children_c = group_children(merged_zone_id);
    check("phase-d-four-children", children_c.size() == 4);
    if (children_c.size() != 4) {
        sim.Stop();
        return failures + 1;
    }
    // Pick the child farthest from the current hotspot (600,150): the NE
    // child in tree order (NW, NE, SW, SE) is children_c[1].
    const auto& target = children_c[1]->bounds;
    const float target_x = (target.min_x + target.max_x) * 0.5f;
    const float target_y = (target.min_y + target.max_y) * 0.5f;
    despawn_range(660, 32);
    spawn_cluster(700, 2300, 12, 0, target_x, target_y);
    const bool populated_d = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 16;
    });
    check("phase-d-populate", populated_d && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(8));
    check("phase-d-no-merge-while-hot", sim.PartitionMetricsSnapshot().merge_commits == 1);
    despawn_range(700, 12);
    const bool merged_again = WaitFor(std::chrono::seconds(90), [&] {
        return sim.PartitionMetricsSnapshot().merge_commits >= 2;
    });
    check("phase-d-merge-after-cold", merged_again && !expired());
    check("phase-d-no-thrash", sim.PartitionMetricsSnapshot().oscillation_guard_trips == 0);
    check("validate-final", validate_now("final"));

    const auto metrics = sim.PartitionMetricsSnapshot();
    std::printf("STABILITY metrics: split_commits=%llu merge_commits=%llu "
                "merge_eval=%llu suppressed=[not_eligible=%llu not_sustained=%llu recent_split=%llu "
                "recent_merge=%llu unsafe=%llu min_improvement=%llu tx=%llu] "
                "split_suppressed_merge_cooldown=%llu emergency_bypass=%llu oscillation=%llu\n",
                (unsigned long long)metrics.split_commits,
                (unsigned long long)metrics.merge_commits,
                (unsigned long long)metrics.merge_candidates_evaluated,
                (unsigned long long)metrics.merge_suppressed_not_eligible,
                (unsigned long long)metrics.merge_suppressed_not_sustained,
                (unsigned long long)metrics.merge_suppressed_recent_split,
                (unsigned long long)metrics.merge_suppressed_recent_merge,
                (unsigned long long)metrics.merge_suppressed_post_merge_unsafe,
                (unsigned long long)metrics.merge_suppressed_min_improvement,
                (unsigned long long)metrics.merge_suppressed_transaction,
                (unsigned long long)metrics.split_suppressed_merge_cooldown,
                (unsigned long long)metrics.split_emergency_bypasses,
                (unsigned long long)metrics.oscillation_guard_trips);
    sim.Stop();
    std::printf("STABILITY-DONE validations=%d failures=%d\n", validations, failures);
    return failures;
}

// Phase 5A ghost correctness scenario: cross-zone ghost add/keep/remove,
// migration (no duplicate / no authoritative ghost), despawn cleanup,
// split/merge topology invalidation, and continuous exact equivalence
// validation (publisher fidelity + reconcile diff) in the supervisor's
// quiescent window. Own sim lifecycle on the test map. Returns failures.
int RunGhostScenario(boost::asio::io_context& io, const BenchConfig& config)
{
    int failures = 0;
    int validations = 0;
    int equivalence_runs = 0;
    int equivalence_failures = 0;
    auto check = [&](const char* name, bool pass) {
        if (pass) {
            std::printf("GHOST %s: PASS\n", name);
        } else {
            std::printf("GHOST %s: FAIL\n", name);
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    auto validate_now = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 100; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("GHOST validation(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("GHOST validation(%s): TIMEOUT\n", what);
        return false;
    };
    auto ghost_equivalence = [&](const char* what) -> bool {
        sim.RequestGhostValidation();
        for (int i = 0; i < 200; ++i) {
            std::string result;
            if (sim.TryTakeGhostValidationResult(result)) {
                ++equivalence_runs;
                if (result != "OK") {
                    ++equivalence_failures;
                    std::printf("GHOST equivalence(%s): FAIL: %s\n", what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("GHOST equivalence(%s): TIMEOUT\n", what);
        ++equivalence_failures;
        return false;
    };
    // Net id of a static resident at an exact position (deterministic).
    auto find_net_at = [&](const gs::game::Zone& zone, float x, float y) -> std::uint32_t {
        for (const auto& [net_id, entity] : zone.Entities()) {
            if (!entity.is_valid() || !entity.has<gs::game::Position>()) {
                continue;
            }
            const auto pos = entity.get<gs::game::Position>();
            if (pos.x == x && pos.y == y) {
                return net_id;
            }
        }
        return 0;
    };
    // Count ghost copies of a net across every zone (must be 0 or 1).
    auto ghost_copies = [&](std::uint32_t net_id) {
        int copies = 0;
        for (std::size_t i = 0; i < sim.Zones().ZoneCount(); ++i) {
            if (sim.Zones().GetZone(i).FindGhost(net_id) != nullptr) {
                ++copies;
            }
        }
        return copies;
    };
    auto ghost_source = [&](std::uint32_t net_id) -> std::uint32_t {
        for (std::size_t i = 0; i < sim.Zones().ZoneCount(); ++i) {
            const auto* ghost = sim.Zones().GetZone(i).FindGhost(net_id);
            if (ghost != nullptr) {
                return ghost->source_zone_id;
            }
        }
        return 0;
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(180, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    // The 500m test-map zones can only split with a lowered floor (validated
    // to 2x AOI radius = 240m).
    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f;
    sim.ConfigurePartition(pcfg);

    sim.Start();

    // ---- Phase 1: cross-zone ghosts (player in zone A, mobs in zone B).
    const std::size_t zone_a_index = sim.Zones().FindIndexForPosition(100.0f, 100.0f);
    const std::size_t zone_b_index = sim.Zones().FindIndexForPosition(800.0f, 100.0f);
    check("zone-pair", zone_a_index < sim.Zones().ZoneCount() &&
                           zone_b_index < sim.Zones().ZoneCount() &&
                           zone_a_index != zone_b_index);
    if (zone_a_index >= sim.Zones().ZoneCount() || zone_b_index >= sim.Zones().ZoneCount()) {
        sim.Stop();
        return failures + 1;
    }
    const gs::game::ZoneId zone_a_id = sim.Zones().GetZone(zone_a_index).Id();
    const gs::game::ZoneId zone_b_id = sim.Zones().GetZone(zone_b_index).Id();

    constexpr float kBoundaryX = 500.0f;
    constexpr float kPlayerY = 250.0f;
    const float player_x = kBoundaryX - 20.0f;
    const float mob_x = kBoundaryX + 20.0f;
    constexpr gs::common::SessionId kPlayer = 770;
    constexpr gs::common::SessionId kPeer = 771;
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPlayer);
        sim.PostSpawn(session, MakeBenchCharacter(970),
                      gs::game::DebugSpawnOverride{player_x, kPlayerY});
    }
    {
        // A viewer on the other side: ghosts exist for zones WITH viewers, so
        // both zones must have a player for the cross-zone ghost assertions.
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPeer);
        sim.PostSpawn(session, MakeBenchCharacter(971),
                      gs::game::DebugSpawnOverride{mob_x, kPlayerY});
    }
    {
        // A second viewer that stays in zone A: after the mobile player
        // migrates east, zone A must still maintain its ghost set.
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), 772);
        sim.PostSpawn(session, MakeBenchCharacter(972),
                      gs::game::DebugSpawnOverride{player_x, kPlayerY + 10.0f});
    }
    for (int i = 0; i < 4; ++i) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = mob_x;
        point.y = kPlayerY - 6.0f + static_cast<float>(i) * 4.0f;
        point.count = 1;
        point.radius = 0.0f; // static mob: exact position forever
        sim.AddMobSpawnPoint(point);
    }
    for (int i = 0; i < 4; ++i) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + i));
    }
    const bool populated = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Owners().size() == 3 && sim.CollectProcessLoad().mobs >= 4;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2)); // publish + reconcile settle

    const auto owner_it = sim.Owners().find(kPlayer);
    check("player-owner", owner_it != sim.Owners().end());
    if (owner_it == sim.Owners().end()) {
        sim.Stop();
        return failures + 1;
    }
    const std::uint32_t player_net = owner_it->second.net_id;
    const const gs::game::Zone& zone_a = sim.Zones().GetZone(zone_a_index);
    const const gs::game::Zone& zone_b = sim.Zones().GetZone(zone_b_index);
    const std::uint32_t mob_nets[4] = {
        find_net_at(zone_b, mob_x, kPlayerY - 6.0f),
        find_net_at(zone_b, mob_x, kPlayerY - 2.0f),
        find_net_at(zone_b, mob_x, kPlayerY + 2.0f),
        find_net_at(zone_b, mob_x, kPlayerY + 6.0f)};
    bool mobs_found = true;
    for (const auto net : mob_nets) {
        mobs_found = mobs_found && net != 0;
    }
    check("mobs-indexed", mobs_found);

    // Zone A must ghost the zone-B mobs; zone B must ghost the zone-A player.
    bool a_ghosts_mobs = mobs_found;
    for (const auto net : mob_nets) {
        const auto* ghost = zone_a.FindGhost(net);
        a_ghosts_mobs = a_ghosts_mobs && ghost != nullptr &&
                        ghost->source_zone_id == zone_b_id &&
                        ghost->snapshot.position.x == mob_x;
    }
    const auto* player_ghost_b = zone_b.FindGhost(player_net);
    check("cross-zone-ghosts", a_ghosts_mobs && player_ghost_b != nullptr &&
                                   player_ghost_b->source_zone_id == zone_a_id);
    check("equivalence-initial", ghost_equivalence("initial"));
    check("validate-initial", validate_now("initial"));
    std::printf("GHOST metrics: zone_a_ghosts=%zu zone_b_ghosts=%zu copies=%d\n",
                zone_a.Ghosts().size(),
                zone_b.Ghosts().size(),
                ghost_copies(player_net));

    // ---- Phase 2: migration across the boundary. During the crossing the
    // player is authoritative in exactly one zone and ghosted in at most one;
    // equivalence must hold throughout.
    std::uint32_t seq = 0;
    const auto cross_until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool crossing_ok = true;
    int max_copies = 0;
    while (std::chrono::steady_clock::now() < cross_until && !expired()) {
        sim.PostMoveInput(kPlayer, ++seq, 1.5707963f, gs::game::MoveState::Running);
        const int copies = ghost_copies(player_net);
        max_copies = std::max(max_copies, copies);
        if (copies > 1) {
            crossing_ok = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    check("migration-no-duplicate", crossing_ok && max_copies <= 1);
    // The player must now be authoritative in B and ghosted in A.
    const auto crossed = WaitFor(std::chrono::seconds(10), [&] {
        const auto it = sim.Owners().find(kPlayer);
        return it != sim.Owners().end() && it->second.zone_index == zone_b_index;
    });
    check("migrated", crossed && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto* player_ghost_a = zone_a.FindGhost(player_net);
    check("migrated-ghost-source", player_ghost_a != nullptr &&
                                      player_ghost_a->source_zone_id == zone_b_id &&
                                      zone_b.FindGhost(player_net) == nullptr);
    check("equivalence-migrated", ghost_equivalence("migrated"));
    check("validate-migrated", validate_now("migrated"));

    // ---- Phase 3: despawn cleanup. No stale ghost may survive.
    sim.PostDespawn(kPlayer);
    const bool drained = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Owners().size() == 2; // both stationary viewers stay
    });
    check("despawn-drained", drained && !expired());
    const bool ghosts_gone = WaitFor(std::chrono::seconds(3), [&] {
        return ghost_copies(player_net) == 0;
    });
    check("despawn-ghost-removed", ghosts_gone);
    check("equivalence-despawn", ghost_equivalence("despawn"));

    // ---- Phase 4: topology invalidation (split + merge). The neighbor sets
    // change; ghosts from former neighbors must be dropped deterministically.
    const std::size_t zones_before = sim.Zones().ZoneCount();
    sim.PostForceSplit(zone_a_id);
    const bool split_done = WaitFor(std::chrono::seconds(15), [&] {
        return sim.Zones().ZoneCount() == zones_before + 4;
    });
    check("split-committed", split_done && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    check("equivalence-after-split", ghost_equivalence("after-split"));
    check("validate-after-split", validate_now("after-split"));

    sim.PostForceMerge(zone_a_id);
    const bool merge_done = WaitFor(std::chrono::seconds(15), [&] {
        const auto metrics = sim.PartitionMetricsSnapshot();
        return metrics.merge_commits >= 1;
    });
    check("merge-committed", merge_done && !expired());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    check("equivalence-after-merge", ghost_equivalence("after-merge"));
    check("validate-final", validate_now("final"));

    const auto ghost_stats = sim.GhostValidationSnapshot();
    std::printf("GHOST stats: equivalence_runs=%d equivalence_failures=%d "
                "validator_runs=%llu validator_failures=%llu repairs=%llu max_copies=%d\n",
                equivalence_runs,
                equivalence_failures,
                (unsigned long long)ghost_stats.runs,
                (unsigned long long)ghost_stats.failures,
                (unsigned long long)ghost_stats.repairs,
                max_copies);
    check("no-validator-failures", ghost_stats.failures == 0 && ghost_stats.repairs == 0);
    check("no-equivalence-failures", equivalence_failures == 0);
    sim.Stop();
    std::printf("GHOST-DONE validations=%d equivalence=%d failures=%d\n",
                validations,
                equivalence_runs,
                failures);
    return failures;
}

// Phase 5B AOI + dirty replication correctness scenario. Own sim lifecycle on
// the test map. Covers:
//   AOI: inside/outside/exact boundary, cross-zone mob + player (ghost
//        backed), viewer self-exclusion, entity move in/out, spawn/despawn
//        inside range, split/merge visibility stability, exact shadow
//        equivalence (brute force vs production interest set).
//   Replication: clean -> no record, changed transform -> record, multi
//        recipient fanout, spawn+transform coalescing, transform+despawn,
//        migration same NetId (no despawn+spawn churn), split/merge no
//        duplicate spawn, recipient coverage (last-sent >= current).
int RunAoiReplicationScenario(boost::asio::io_context& io,
                              const BenchConfig& config,
                              const char* tag)
{
    int failures = 0;
    int validations = 0;
    int shadow_runs = 0;
    int shadow_failures = 0;
    auto check = [&](const char* name, bool pass) {
        std::printf("%s %s: %s\n", tag, name, pass ? "PASS" : "FAIL");
        if (!pass) {
            ++failures;
        }
    };

    gs::game::WorldRuntime sim(io);
    // Long refresh period: the "clean -> suppressed" window must be
    // observable, and the staggered refresh must not interfere with the
    // dirty assertions. Production default is 20 ticks (1 s).
    gs::game::ReplicationConfig repl_cfg;
    repl_cfg.dirty_enabled = true;
    repl_cfg.aoi_partial_cap = true;
    repl_cfg.refresh_ticks = 400;
    sim.ConfigureReplication(repl_cfg);
    // The 500 m test zones can split with the validated 240 m floor.
    gs::game::PartitionConfig pcfg;
    pcfg.min_zone_size_m = 100.0f;
    sim.ConfigurePartition(pcfg);

    auto shadow_now = [&](const char* what) -> bool {
        sim.RequestReplicationValidation();
        for (int i = 0; i < 300; ++i) {
            std::string result;
            if (sim.TryTakeReplicationValidationResult(result)) {
                ++shadow_runs;
                if (result != "OK") {
                    ++shadow_failures;
                    std::printf("%s shadow(%s): FAIL: %s\n", tag, what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("%s shadow(%s): TIMEOUT\n", tag, what);
        ++shadow_failures;
        return false;
    };
    auto world_validate = [&](const char* what) -> bool {
        sim.RequestValidation();
        for (int i = 0; i < 300; ++i) {
            std::string result;
            if (sim.TryTakeValidationResult(result)) {
                ++validations;
                if (result != "OK") {
                    std::printf("%s world-validation(%s): FAIL: %s\n", tag, what, result.c_str());
                    return false;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("%s world-validation(%s): TIMEOUT\n", tag, what);
        return false;
    };

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(180, config.seconds));
    auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };

    constexpr gs::common::SessionId kV1 = 780;      // viewer in zone A
    constexpr gs::common::SessionId kV2 = 781;      // second viewer in zone A
    constexpr gs::common::SessionId kPCross = 782;  // visible player in zone B
    constexpr gs::common::SessionId kP2 = 783;      // mover in zone B
    constexpr gs::common::SessionId kP3 = 784;      // spawn/despawn inside range
    constexpr gs::common::SessionId kP4 = 785;      // spawn + dirty coalescing

    // Viewer at (420,420): 120 m from the x=500 border, 172+ m from the
    // nearest map base spawn (so only our controlled entities are in AOI).
    constexpr float kViewerX = 420.0f;
    constexpr float kViewerY = 420.0f;
    constexpr float kMobInX = 460.0f;    // 40 m
    constexpr float kMobCrossX = 540.0f; // exactly 120 m (zone B, ghost path)
    constexpr float kMobOutX = 545.0f;   // 125 m (zone B, outside exact AOI)
    constexpr float kPCrossX = 530.0f;   // 110 m (zone B, player ghost)
    constexpr float kP2X = 600.0f;       // 180 m (zone B, starts invisible)

    sim.Start();

    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kV1);
        sim.PostSpawn(session, MakeBenchCharacter(980),
                      gs::game::DebugSpawnOverride{kViewerX, kViewerY});
    }
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kV2);
        sim.PostSpawn(session, MakeBenchCharacter(981),
                      gs::game::DebugSpawnOverride{kViewerX, kViewerY + 10.0f});
    }
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kPCross);
        sim.PostSpawn(session, MakeBenchCharacter(982),
                      gs::game::DebugSpawnOverride{kPCrossX, kViewerY});
    }
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kP2);
        sim.PostSpawn(session, MakeBenchCharacter(983),
                      gs::game::DebugSpawnOverride{kP2X, kViewerY});
    }
    // Static mobs (radius 0): exact positions forever, so any transform
    // record for them is a correctness signal.
    const float mob_x[3] = {kMobInX, kMobCrossX, kMobOutX};
    for (int i = 0; i < 3; ++i) {
        gs::game::MobSpawnPoint point;
        point.mob_type_id = 2;
        point.x = mob_x[i];
        point.y = kViewerY;
        point.count = 1;
        point.radius = 0.0f;
        sim.AddMobSpawnPoint(point);
    }
    for (int i = 0; i < 3; ++i) {
        sim.RequestMobSpawn(static_cast<std::size_t>(4 + i));
    }

    const bool populated = WaitFor(std::chrono::seconds(25), [&] {
        return sim.Owners().size() == 4 && sim.CollectProcessLoad().mobs >= 3;
    });
    check("populate", populated && !expired());
    if (!populated) {
        sim.Stop();
        return failures + 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2)); // publish + reconcile settle

    auto net_of = [&](gs::common::SessionId session) -> std::uint32_t {
        const auto it = sim.Owners().find(session);
        return it != sim.Owners().end() ? it->second.net_id : 0;
    };
    auto zone_of = [&](gs::common::SessionId session) -> const gs::game::Zone* {
        const auto it = sim.Owners().find(session);
        if (it == sim.Owners().end() || it->second.zone_index >= sim.Zones().ZoneCount()) {
            return nullptr;
        }
        return &sim.Zones().GetZone(it->second.zone_index);
    };
    auto visible = [&](gs::common::SessionId session, std::uint32_t net_id) -> bool {
        const gs::game::Zone* zone = zone_of(session);
        if (zone == nullptr) {
            return false;
        }
        const auto* binding = zone->FindPlayer(net_of(session));
        return binding != nullptr && binding->IsVisible(net_id);
    };
    auto visible_count = [&](gs::common::SessionId session) -> std::size_t {
        const gs::game::Zone* zone = zone_of(session);
        if (zone == nullptr) {
            return 0;
        }
        const auto* binding = zone->FindPlayer(net_of(session));
        return binding != nullptr ? binding->visible_net_versions.size() : 0;
    };
    auto visible_set = [&](gs::common::SessionId session) {
        std::vector<std::uint32_t> nets;
        const gs::game::Zone* zone = zone_of(session);
        if (zone == nullptr) {
            return nets;
        }
        const auto* binding = zone->FindPlayer(net_of(session));
        if (binding == nullptr) {
            return nets;
        }
        nets.reserve(binding->visible_net_versions.size());
        for (const auto& [net_id, version] : binding->visible_net_versions) {
            (void)version;
            nets.push_back(net_id);
        }
        std::sort(nets.begin(), nets.end());
        return nets;
    };
    auto find_net_at = [&](const gs::game::Zone& zone, float x, float y) -> std::uint32_t {
        for (const auto& [net_id, entity] : zone.Entities()) {
            if (!entity.is_valid() || !entity.has<gs::game::Position>()) {
                continue;
            }
            const auto pos = entity.get<gs::game::Position>();
            if (pos.x == x && pos.y == y) {
                return net_id;
            }
        }
        return 0;
    };
    // Recipient-level lifecycle counters (travel with the binding across
    // topology changes): {spawns, despawns, updates}.
    auto recipient_events = [&](gs::common::SessionId session) {
        std::array<std::uint64_t, 3> events{0, 0, 0};
        const gs::game::Zone* zone = zone_of(session);
        if (zone == nullptr) {
            return events;
        }
        const auto* binding = zone->FindPlayer(net_of(session));
        if (binding == nullptr) {
            return events;
        }
        events[0] = binding->spawn_events;
        events[1] = binding->despawn_events;
        events[2] = binding->update_events;
        return events;
    };

    const std::uint32_t v1_net = net_of(kV1);
    const std::uint32_t p_cross_net = net_of(kPCross);
    const std::uint32_t p2_net = net_of(kP2);
    check("nets-resolved", v1_net != 0 && p_cross_net != 0 && p2_net != 0);
    if (v1_net == 0 || p_cross_net == 0 || p2_net == 0) {
        sim.Stop();
        return failures + 1;
    }
    const std::size_t zone_a_index = sim.Zones().FindIndexForPosition(kViewerX, kViewerY);
    const std::size_t zone_b_index = sim.Zones().FindIndexForPosition(kPCrossX, kViewerY);
    const gs::game::ZoneId zone_a_id = sim.Zones().GetZone(zone_a_index).Id();
    const gs::game::Zone& zone_a = sim.Zones().GetZone(zone_a_index);
    const gs::game::Zone& zone_b = sim.Zones().GetZone(zone_b_index);
    const std::uint32_t m_in_net = find_net_at(zone_a, kMobInX, kViewerY);
    const std::uint32_t m_cross_net = find_net_at(zone_b, kMobCrossX, kViewerY);
    const std::uint32_t m_out_net = find_net_at(zone_b, kMobOutX, kViewerY);
    check("static-mobs-resolved", m_in_net != 0 && m_cross_net != 0 && m_out_net != 0);

    // ---- AOI exact semantics -------------------------------------------
    check("aoi-inside-visible", visible(kV1, m_in_net));
    check("aoi-exact-boundary-visible", visible(kV1, m_cross_net)); // 120.0 m
    check("aoi-outside-invisible", !visible(kV1, m_out_net));       // 125 m
    check("aoi-cross-zone-mob-visible", visible(kV1, m_cross_net)); // ghost backed
    check("aoi-cross-zone-player-visible", visible(kV1, p_cross_net));
    check("aoi-viewer-self-excluded", !visible(kV1, v1_net));
    check("aoi-second-viewer-consistent", visible(kV2, m_in_net) && visible(kV2, p_cross_net));
    check("shadow-initial", shadow_now("initial"));

    // ---- entity moves into range, then out ------------------------------
    std::uint32_t seq = 0;
    const auto enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool entered = false;
    while (std::chrono::steady_clock::now() < enter_deadline && !entered && !expired()) {
        sim.PostMoveInput(kP2, ++seq, -1.5707963f, gs::game::MoveState::Running);
        entered = visible(kV1, p2_net);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check("entity-enters-range", entered && !expired());
    const auto leave_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool left = false;
    while (std::chrono::steady_clock::now() < leave_deadline && !left && !expired()) {
        sim.PostMoveInput(kP2, ++seq, 1.5707963f, gs::game::MoveState::Running);
        left = !visible(kV1, p2_net);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check("entity-leaves-range", left && !expired());
    check("shadow-after-entity-move", shadow_now("after-entity-move"));

    // ---- spawn inside range / despawn inside range ----------------------
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kP3);
        sim.PostSpawn(session, MakeBenchCharacter(984),
                      gs::game::DebugSpawnOverride{kViewerX + 40.0f, kViewerY + 40.0f});
    }
    const bool p3_visible = WaitFor(std::chrono::seconds(10), [&] {
        return net_of(kP3) != 0 && visible(kV1, net_of(kP3));
    });
    check("spawn-inside-visible", p3_visible && !expired());
    sim.PostDespawn(kP3);
    const bool p3_gone = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Owners().find(kP3) == sim.Owners().end();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check("despawn-inside-removed", p3_gone && !visible(kV1, net_of(kP3) == 0 ? 0 : net_of(kP3)));
    check("shadow-after-spawn-despawn", shadow_now("after-spawn-despawn"));

    // ---- dirty replication: clean entities produce no records ------------
    const gs::game::Zone& v1_zone = *zone_of(kV1);
    auto counters = [&](const gs::game::Zone& zone) {
        return std::array<std::uint64_t, 4>{
            zone.Diagnostics().repl_update_since_diag.load(std::memory_order_relaxed),
            zone.Diagnostics().repl_spawn_since_diag.load(std::memory_order_relaxed),
            zone.Diagnostics().repl_despawn_since_diag.load(std::memory_order_relaxed),
            zone.Diagnostics().repl_suppressed_since_diag.load(std::memory_order_relaxed)};
    };
    const auto clean_before = counters(v1_zone);
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // ~12 ticks
    const auto clean_after = counters(v1_zone);
    check("dirty-clean-no-update", clean_after[0] == clean_before[0]);
    check("dirty-clean-no-lifecycle",
          clean_after[1] == clean_before[1] && clean_after[2] == clean_before[2]);
    check("dirty-clean-suppressed", clean_after[3] > clean_before[3]);

    // ---- changed transform -> update; both recipients catch up -----------
    const auto dirty_before = counters(v1_zone);
    seq = 0;
    const auto dirty_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < dirty_deadline && !expired()) {
        sim.PostMoveInput(kPCross, ++seq, -1.5707963f, gs::game::MoveState::Walking);
        const auto now = counters(v1_zone);
        if (now[0] > dirty_before[0]) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto dirty_after = counters(v1_zone);
    check("dirty-changed-update", dirty_after[0] > dirty_before[0]);
    check("dirty-multi-recipient-coverage", shadow_now("multi-recipient"));

    // ---- spawn + dirty transform coalescing (spawn carries the state) ----
    {
        boost::asio::ip::tcp::socket socket(io);
        auto session = std::make_shared<gs::network::Session>(std::move(socket), kP4);
        sim.PostSpawn(session, MakeBenchCharacter(985),
                      gs::game::DebugSpawnOverride{kViewerX + 30.0f, kViewerY + 30.0f});
    }
    const bool p4_visible = WaitFor(std::chrono::seconds(10), [&] {
        return net_of(kP4) != 0 && visible(kV1, net_of(kP4));
    });
    seq = 0;
    for (int i = 0; i < 4; ++i) {
        sim.PostMoveInput(kP4, ++seq, 0.7853982f, gs::game::MoveState::Walking);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check("spawn-plus-dirty-coalesced", p4_visible && shadow_now("spawn-plus-dirty"));

    // ---- dirty transform + despawn -> no stale interest ------------------
    sim.PostDespawn(kP4);
    const bool p4_gone = WaitFor(std::chrono::seconds(10), [&] {
        return sim.Owners().find(kP4) == sim.Owners().end();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check("dirty-despawn-clean", p4_gone && shadow_now("dirty-despawn"));

    // ---- migration: same NetId, no despawn+spawn churn -------------------
    const auto mig_before = counters(v1_zone);
    const bool p_cross_visible_before = visible(kV1, p_cross_net);
    seq = 0;
    const auto mig_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool p_cross_migrated = false;
    while (std::chrono::steady_clock::now() < mig_deadline && !p_cross_migrated && !expired()) {
        sim.PostMoveInput(kPCross, ++seq, -1.5707963f, gs::game::MoveState::Walking);
        const auto owner = sim.Owners().find(kPCross);
        if (owner != sim.Owners().end() && owner->second.zone_index == zone_a_index) {
            p_cross_migrated = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto mig_after = counters(v1_zone);
    check("migration-same-net",
          p_cross_visible_before && p_cross_migrated && visible(kV1, p_cross_net));
    check("migration-no-spawn-despawn-churn",
          mig_after[1] == mig_before[1] && mig_after[2] == mig_before[2]);
    check("shadow-after-migration", shadow_now("after-migration"));

    // ---- split / merge: visibility must not change with topology ---------
    const auto set_before_split = visible_set(kV1);
    const auto split_events_before = recipient_events(kV1);
    const std::size_t zones_before = sim.Zones().ZoneCount();
    sim.PostForceSplit(zone_a_id);
    const bool split_done = WaitFor(std::chrono::seconds(20), [&] {
        return sim.Zones().ZoneCount() == zones_before + 4;
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto set_after_split = visible_set(kV1);
    const auto split_events_after = recipient_events(kV1);
    check("split-committed", split_done && !expired());
    check("split-visibility-stable", set_before_split == set_after_split);
    if (split_events_after[0] != split_events_before[0] ||
        split_events_after[1] != split_events_before[1]) {
        std::printf("%s split-churn detail: before=[spawn=%llu despawn=%llu upd=%llu] "
                    "after=[spawn=%llu despawn=%llu upd=%llu] set=%zu/%zu\n",
                    tag,
                    (unsigned long long)split_events_before[0],
                    (unsigned long long)split_events_before[1],
                    (unsigned long long)split_events_before[2],
                    (unsigned long long)split_events_after[0],
                    (unsigned long long)split_events_after[1],
                    (unsigned long long)split_events_after[2],
                    set_before_split.size(),
                    set_after_split.size());
    }
    check("split-no-spawn-despawn-churn",
          split_events_after[0] == split_events_before[0] &&
              split_events_after[1] == split_events_before[1]);
    check("shadow-after-split", shadow_now("after-split"));

    sim.PostForceMerge(zone_a_id);
    const bool merge_done = WaitFor(std::chrono::seconds(20), [&] {
        return sim.PartitionMetricsSnapshot().merge_commits >= 1;
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto set_after_merge = visible_set(kV1);
    check("merge-committed", merge_done && !expired());
    check("merge-visibility-stable", set_before_split == set_after_merge);
    check("shadow-after-merge", shadow_now("after-merge"));
    check("world-validation-final", world_validate("final"));

    const auto shadow_stats = sim.ReplicationValidationSnapshot();
    std::printf("%s stats: shadow_runs=%d shadow_failures=%d validator_runs=%llu "
                "validator_failures=%llu visible=%zu\n",
                tag,
                shadow_runs,
                shadow_failures,
                (unsigned long long)shadow_stats.runs,
                (unsigned long long)shadow_stats.failures,
                visible_count(kV1));
    check("no-shadow-failures", shadow_stats.failures == 0 && shadow_failures == 0);
    check("no-validator-failures", sim.GhostValidationSnapshot().failures == 0);
    sim.Stop();
    std::printf("%s-DONE validations=%d shadow=%d failures=%d\n",
                tag,
                validations,
                shadow_runs,
                failures);
    return failures;
}

} // namespace

int BenchMain(int argc, char** argv)
{
    BenchConfig config;
    if (!ParseArgs(argc, argv, config)) {
        return 1;
    }

    gs::common::InitLogging("warning", "logs/world_bench.log");

    // Pure field checks need no world, no map and no threads: run and exit.
    if (config.field_selftest) {
        return RunFieldSelftest() == 0 ? 0 : 1;
    }
    if (config.load_field_selftest) {
        return RunLoadFieldSelftest() == 0 ? 0 : 1;
    }
    if (config.partition_score_selftest) {
        return RunPartitionScoreSelftest() == 0 ? 0 : 1;
    }

    std::printf("worldbench: players=%d mobs=%d seconds=%d mode=%s validate_every=%d "
                "despawn_storm=%d seed=%u logical_processes=%d routing_selftest=%d\n",
                config.players,
                config.mobs,
                config.seconds,
                config.mode.c_str(),
                config.validate_every,
                config.despawn_storm,
                config.seed,
                config.logical_processes,
                config.routing_selftest ? 1 : 0);

    if (config.mode == "splitmerge") {
        // Deterministic transaction scenario with its own sim lifecycle;
        // the load-loop below is skipped entirely for this mode.
        boost::asio::io_context splitmerge_io;
        std::thread splitmerge_io_thread([&splitmerge_io] { splitmerge_io.run(); });
        const int scenario_failures = RunSplitMergeScenario(splitmerge_io, config);
        splitmerge_io.stop();
        if (splitmerge_io_thread.joinable()) {
            splitmerge_io_thread.join();
        }
        std::printf("BENCH-DONE splitmerge failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "lod") {
        // Simulation LOD correctness scenario with its own sim lifecycle.
        boost::asio::io_context lod_io;
        std::thread lod_io_thread([&lod_io] { lod_io.run(); });
        const int scenario_failures = RunLodScenario(lod_io, config);
        lod_io.stop();
        if (lod_io_thread.joinable()) {
            lod_io_thread.join();
        }
        std::printf("BENCH-DONE lod failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "activity") {
        // Cross-zone LOD determinism via the activity field.
        boost::asio::io_context activity_io;
        std::thread activity_io_thread([&activity_io] { activity_io.run(); });
        const int scenario_failures = RunActivityScenario(activity_io, config);
        activity_io.stop();
        if (activity_io_thread.joinable()) {
            activity_io_thread.join();
        }
        std::printf("BENCH-DONE activity failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "loadfield") {
        // Continuous multi-channel load field scenario + resolution sweep.
        boost::asio::io_context loadfield_io;
        std::thread loadfield_io_thread([&loadfield_io] { loadfield_io.run(); });
        const int scenario_failures = RunLoadFieldScenario(loadfield_io, config);
        loadfield_io.stop();
        if (loadfield_io_thread.joinable()) {
            loadfield_io_thread.join();
        }
        std::printf("BENCH-DONE loadfield failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "partitionscore") {
        // Adaptive partition scoring + hotspot-aware split scenario.
        boost::asio::io_context score_io;
        std::thread score_io_thread([&score_io] { score_io.run(); });
        const int scenario_failures = RunPartitionScoreScenario(score_io, config);
        score_io.stop();
        if (score_io_thread.joinable()) {
            score_io_thread.join();
        }
        std::printf("BENCH-DONE partitionscore failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "stability") {
        // Merge scoring + sustained-low + cooldown/oscillation stability.
        boost::asio::io_context stability_io;
        std::thread stability_io_thread([&stability_io] { stability_io.run(); });
        const int scenario_failures = RunStabilityScenario(stability_io, config);
        stability_io.stop();
        if (stability_io_thread.joinable()) {
            stability_io_thread.join();
        }
        std::printf("BENCH-DONE stability failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "ghost") {
        // Phase 5A ghost incremental maintenance correctness scenario.
        boost::asio::io_context ghost_io;
        std::thread ghost_io_thread([&ghost_io] { ghost_io.run(); });
        const int scenario_failures = RunGhostScenario(ghost_io, config);
        ghost_io.stop();
        if (ghost_io_thread.joinable()) {
            ghost_io_thread.join();
        }
        std::printf("BENCH-DONE ghost failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "aoi" || config.mode == "replication") {
        // Phase 5B AOI + dirty replication correctness scenario (one rig
        // covers both; the mode only selects the report prefix).
        boost::asio::io_context aoi_io;
        std::thread aoi_io_thread([&aoi_io] { aoi_io.run(); });
        const char* tag = config.mode == "aoi" ? "AOI" : "REPL";
        const int scenario_failures = RunAoiReplicationScenario(aoi_io, config, tag);
        aoi_io.stop();
        if (aoi_io_thread.joinable()) {
            aoi_io_thread.join();
        }
        std::printf("BENCH-DONE %s failures=%d\n", config.mode.c_str(), scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    if (config.mode == "readiness") {
        // Phase-4 integrated readiness benchmark (synthetic 100km world).
        gs::bench::ReadinessConfig readiness;
        readiness.scenario = config.scenario;
        readiness.world_km = config.world_km;
        readiness.zones_x = config.zones_x;
        readiness.zones_y = config.zones_y;
        readiness.players = config.players;
        readiness.mobs = config.mobs;
        readiness.warmup_seconds = config.warmup_seconds;
        readiness.measure_seconds = config.seconds;
        readiness.asf_off = config.asf_off;
        readiness.load_field_off = config.load_field_off;
        readiness.lod_off = config.lod_off;
        readiness.ghost_shadow = config.ghost_shadow;
        readiness.replication_shadow = config.replication_shadow;
        readiness.repl_full = config.repl_full;
        readiness.aoi_full_sort = config.aoi_full_sort;
        readiness.seed = config.seed;
        boost::asio::io_context readiness_io;
        std::thread readiness_io_thread([&readiness_io] { readiness_io.run(); });
        const int scenario_failures = gs::bench::RunReadinessBenchmark(readiness_io, readiness);
        readiness_io.stop();
        if (readiness_io_thread.joinable()) {
            readiness_io_thread.join();
        }
        std::printf("BENCH-DONE readiness failures=%d\n", scenario_failures);
        return scenario_failures == 0 ? 0 : 2;
    }

    std::mt19937 rng(config.seed);

    boost::asio::io_context io;
    std::thread io_thread([&io] {
        io.run();
    });

    int validation_failures = 0;
    int validations_run = 0;
    std::uint64_t start_deaths = 0;
    std::uint64_t start_attacks = 0;

    {
        gs::game::WorldRuntime sim(io);
        // All configuration is applied pre-Start: zone-bound resources (LOD
        // switch, load-bin mapping, partition limits) must not be rebound
        // while a worker tick can be in flight.
        if (config.lod_off) {
            gs::game::LodConfig lod;
            lod.enabled = false;
            sim.ConfigureSimulationLod(lod);
            std::printf("simulation lod: DISABLED (legacy every-tick behavior)\n");
        }
        if (config.load_field_off) {
            gs::game::LoadFieldConfig load_field;
            load_field.enabled = false;
            sim.ConfigureLoadField(load_field);
            std::printf("continuous load field: DISABLED (instrumentation A/B baseline)\n");
        }
        if (config.partition_min_size > 0) {
            gs::game::PartitionConfig override;
            override.min_zone_size_m = static_cast<float>(config.partition_min_size);
            sim.ConfigurePartition(override);
            std::printf("partition override: min_zone_size_m=%d (effective %.0f after validation)\n",
                        config.partition_min_size,
                        sim.EffectivePartitionConfig().min_zone_size_m);
        }
        if (config.logical_processes >= 2) {
            sim.EmulateDistribution(static_cast<std::uint32_t>(config.logical_processes));
            std::printf("logical distribution: %d processes emulated\n", config.logical_processes);
        }
        sim.Start();
        if (config.routing_selftest) {
            validation_failures += RunRoutingSelftest(sim, io, config);
        }
        start_deaths = sim.DeathsTotal();
        start_attacks = sim.AttacksTotal();

        // Extra mob spawn points (type 2 goblins), injected over time. In
        // hotspot/dense modes they cluster at the player anchors so melee
        // attacks land and the kill/respawn path is exercised.
        static constexpr float kAnchors[4][2] = {
            {98.0f, 88.0f}, {240.0f, 123.0f}, {254.0f, 261.0f}, {381.0f, 248.0f}};
        const int base_spawn_points = 4; // from mob_spawns.conf
        for (int i = 0; i < config.mobs; ++i) {
            gs::game::MobSpawnPoint point;
            point.mob_type_id = 2;
            if (config.mode == "hotspot" || config.mode == "dense") {
                point.x = kAnchors[i % 4][0] + static_cast<float>((i * 37) % 10);
                point.y = kAnchors[i % 4][1] + static_cast<float>((i * 53) % 10);
            } else {
                point.x = 50.0f + static_cast<float>((i * 37) % 900);
                point.y = 50.0f + static_cast<float>((i * 53) % 900);
            }
            point.count = 1;
            point.radius = 5.0f;
            sim.AddMobSpawnPoint(point);
        }

        // Players.
        std::vector<BenchClient> clients(static_cast<std::size_t>(config.players));
        std::uniform_real_distribution<float> heading_dist(0.0f, 6.2831853f);
        for (int i = 0; i < config.players; ++i) {
            boost::asio::ip::tcp::socket socket(io);
            const auto session_id = static_cast<gs::common::SessionId>(100 + i);
            clients[static_cast<std::size_t>(i)].session =
                std::make_shared<gs::network::Session>(std::move(socket), session_id);
            clients[static_cast<std::size_t>(i)].session_id = session_id;
            clients[static_cast<std::size_t>(i)].alive = true;
            clients[static_cast<std::size_t>(i)].heading = heading_dist(rng);
            clients[static_cast<std::size_t>(i)].slot = i;

            float sx = 100.0f;
            float sy = 100.0f;
            if (config.mode == "spread") {
                sx = 50.0f + static_cast<float>((i * 71) % 900);
                sy = 50.0f + static_cast<float>((i * 97) % 900);
            } else if (config.mode == "hotspot" || config.mode == "dense") {
                // Co-locate with mob spawn anchors so attacks land and
                // kills/respawns are exercised.
                sx = kAnchors[i % 4][0] + static_cast<float>(i % 7);
                sy = kAnchors[i % 4][1] + static_cast<float>((i / 7) % 7);
            } else if (config.mode == "border") {
                sx = 460.0f + static_cast<float>(i % 30);
                sy = 200.0f + static_cast<float>((i * 13) % 200);
                clients[static_cast<std::size_t>(i)].heading = 1.5707963f; // east
            }
            sim.PostSpawn(clients[static_cast<std::size_t>(i)].session,
                          MakeBenchCharacter(i),
                          gs::game::DebugSpawnOverride{sx, sy});
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(config.seconds);
        auto next_mob_batch = std::chrono::steady_clock::now();
        auto next_move = std::chrono::steady_clock::now();
        auto next_attack = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto next_storm = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto next_validate = std::chrono::steady_clock::now() + std::chrono::seconds(config.validate_every);
        auto next_progress = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int mobs_queued = 0;
        int next_session_id = 100 + config.players;
        std::uint32_t mob_net_hi = static_cast<std::uint32_t>(1'000'000 + 9 + config.mobs + 1000);

        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();

            // Staggered mob injection (avoids one giant spawn spike).
            if (mobs_queued < config.mobs && now >= next_mob_batch) {
                const int batch =
                    std::min(config.mobs - mobs_queued, std::max(1, config.mobs / 10));
                for (int k = 0; k < batch; ++k) {
                    sim.RequestMobSpawn(static_cast<std::size_t>(base_spawn_points + mobs_queued));
                    ++mobs_queued;
                }
                next_mob_batch = now + std::chrono::seconds(1);
            }

            // Movement inputs ~10 Hz per alive player. Hotspot/dense clients
            // hold position so melee attacks land and kills/respawns happen;
            // other modes roam.
            if (now >= next_move) {
                const bool stationary = config.mode == "hotspot" || config.mode == "dense";
                for (auto& client : clients) {
                    if (!client.alive || stationary) {
                        continue;
                    }
                    if (config.mode == "spread" && (rng() % 20) == 0) {
                        client.heading = heading_dist(rng);
                    }
                    sim.PostMoveInput(client.session_id,
                                      ++client.seq,
                                      client.heading,
                                      gs::game::MoveState::Running);
                }
                next_move = now + std::chrono::milliseconds(100);
            }

            // Attacks. In hotspot/dense modes each client round-robins the
            // extra mobs spawned at its own anchor, so hits land regularly;
            // other modes pick uniformly (mostly exercising routing/rejects).
            if (now >= next_attack) {
                std::uniform_int_distribution<std::uint32_t> mob_pick(1'000'000, mob_net_hi);
                const int per_anchor =
                    (config.mobs > 0) ? (config.mobs + 3) / 4 : 0;
                const bool targeted =
                    (config.mode == "hotspot" || config.mode == "dense") && per_anchor > 0;
                int attackers = 0;
                for (auto& client : clients) {
                    if (!client.alive) {
                        continue;
                    }
                    const bool attack =
                        config.mode == "spread" ? (rng() % 20 == 0) : (rng() % 3 == 0);
                    if (!attack) {
                        continue;
                    }
                    std::uint32_t target = mob_pick(rng);
                    if (targeted) {
                        const int j = client.target_cursor++ % per_anchor;
                        target = static_cast<std::uint32_t>(
                            1'000'009 + ((client.slot % 4) + 4 * j) % config.mobs);
                    }
                    sim.PostAttackTarget(client.session_id, target);
                    if (++attackers > 200) {
                        break;
                    }
                }
                next_attack = now + std::chrono::milliseconds(500);
            }

            // Despawn storm: kill + respawn random players (failure injection,
            // incl. mid-migration disconnects in border mode).
            if (config.despawn_storm > 0 && now >= next_storm) {
                for (int k = 0; k < config.despawn_storm && !clients.empty(); ++k) {
                    const std::size_t victim = rng() % clients.size();
                    auto& client = clients[victim];
                    if (client.alive) {
                        sim.PostDespawn(client.session_id);
                        client.alive = false;
                    } else {
                        boost::asio::ip::tcp::socket socket(io);
                        const auto session_id =
                            static_cast<gs::common::SessionId>(next_session_id++);
                        client.session =
                            std::make_shared<gs::network::Session>(std::move(socket), session_id);
                        client.session_id = session_id;
                        client.alive = true;
                        client.seq = 0;
                        sim.PostSpawn(client.session,
                                      MakeBenchCharacter(client.slot),
                                      gs::game::DebugSpawnOverride{100.0f, 100.0f});
                    }
                }
                next_storm = now + std::chrono::seconds(1);
            }

            // Periodic consistency validation (supervisor-side quiescent window).
            if (config.validate_every > 0 && now >= next_validate) {
                sim.RequestValidation();
                next_validate = now + std::chrono::seconds(config.validate_every);
            }
            std::string validation_result;
            while (sim.TryTakeValidationResult(validation_result)) {
                ++validations_run;
                if (validation_result != "OK") {
                    ++validation_failures;
                    std::printf("VALIDATION FAIL: %s\n", validation_result.c_str());
                }
            }

            if (now >= next_progress) {
                const auto mig_stats = sim.MigrationMetrics();
                std::printf("  t=%us tick=%u deaths=%llu attacks=%llu mig_pending=%zu quarantined=%zu "
                            "mig_committed=%llu mig_dup=%llu\n",
                            config.seconds -
                                static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                     deadline - now)
                                                     .count()),
                            sim.WorldTick(),
                            (unsigned long long)(sim.DeathsTotal() - start_deaths),
                            (unsigned long long)(sim.AttacksTotal() - start_attacks),
                            sim.Migrations().PendingCount(),
                            sim.MigrationQuarantined(),
                            (unsigned long long)mig_stats.committed,
                            (unsigned long long)mig_stats.duplicates);
                next_progress = now + std::chrono::seconds(5);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Final validation round.
        sim.RequestValidation();
        for (int i = 0; i < 50; ++i) {
            std::string validation_result;
            if (sim.TryTakeValidationResult(validation_result)) {
                ++validations_run;
                if (validation_result != "OK") {
                    ++validation_failures;
                    std::printf("VALIDATION FAIL: %s\n", validation_result.c_str());
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Aggregate tick samples across zones.
        std::vector<std::uint64_t> all_samples;
        std::uint64_t max_sample = 0;
        std::uint64_t sum_samples = 0;
        struct ZoneAvg {
            std::uint32_t zone_id = 0;
            double avg_ms = 0.0;
            std::size_t samples = 0;
        };
        std::vector<ZoneAvg> zone_avgs;
        for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
            const auto& zone = sim.Zones().GetZone(zi);
            std::uint64_t buffer[gs::game::ZoneDiagnostics::kTickSampleCapacity];
            const std::size_t n = zone.Diagnostics().CopyTickSamples(buffer, 256);
            std::uint64_t zone_sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                all_samples.push_back(buffer[i]);
                zone_sum += buffer[i];
                max_sample = std::max(max_sample, buffer[i]);
                sum_samples += buffer[i];
            }
            zone_avgs.push_back(
                ZoneAvg{zone.Id(), n > 0 ? static_cast<double>(zone_sum) / n / 1000.0 : 0.0, n});
        }
        std::sort(zone_avgs.begin(), zone_avgs.end(), [](const ZoneAvg& a, const ZoneAvg& b) {
            return a.avg_ms > b.avg_ms;
        });

        const auto worker_util = sim.WorkerUtilization();
        std::printf("\n==== worldbench report ====\n");
        std::printf("config: players=%d mobs=%d seconds=%d mode=%s\n",
                    config.players,
                    config.mobs,
                    config.seconds,
                    config.mode.c_str());
        std::printf("zone ticks: samples=%zu avg=%.3fms p50=%.3fms p95=%.3fms p99=%.3fms max=%.3fms\n",
                    all_samples.size(),
                    all_samples.empty() ? 0.0
                                        : static_cast<double>(sum_samples) / all_samples.size() / 1000.0,
                    Percentile(all_samples, 50.0),
                    Percentile(all_samples, 95.0),
                    Percentile(all_samples, 99.0),
                    static_cast<double>(max_sample) / 1000.0);
        std::printf("busiest zones (avg ms):");
        for (std::size_t i = 0; i < std::min<std::size_t>(zone_avgs.size(), 5); ++i) {
            std::printf(" [zone=%u %.3fms n=%zu]", zone_avgs[i].zone_id, zone_avgs[i].avg_ms,
                        zone_avgs[i].samples);
        }
        std::printf("\n");
        std::printf("workers: tasks=%llu busy=%.2fs supervisor_avg=%.3fms\n",
                    (unsigned long long)worker_util.tasks_completed,
                    static_cast<double>(worker_util.busy_micros) / 1'000'000.0,
                    sim.SupervisorAvgMs());
        std::printf("gameplay: deaths=%llu attacks=%llu mig_pending=%zu quarantined=%zu\n",
                    (unsigned long long)(sim.DeathsTotal() - start_deaths),
                    (unsigned long long)(sim.AttacksTotal() - start_attacks),
                    sim.Migrations().PendingCount(),
                    sim.MigrationQuarantined());
        std::printf("validation: runs=%d failures=%d\n", validations_run, validation_failures);
        const auto process_load = sim.CollectProcessLoad();
        const auto routes = sim.Router().MetricsSnapshot();
        std::printf("process load: node=%u process=%u tick=%u zones=%zu active=%zu sleeping=%zu "
                    "players=%llu mobs=%llu ghosts=%llu avg_tick=%.3fms repl=%llu mig=%llu "
                    "worker_tasks=%llu worker_busy_us=%llu sup_avg=%.3fms\n",
                    process_load.identity.node.value, process_load.identity.process.value,
                    process_load.world_tick, process_load.zone_count, process_load.active_zones,
                    process_load.sleeping_zones, (unsigned long long)process_load.players,
                    (unsigned long long)process_load.mobs, (unsigned long long)process_load.ghosts,
                    process_load.avg_zone_tick_ms, (unsigned long long)process_load.repl_records,
                    (unsigned long long)process_load.migrations,
                    (unsigned long long)process_load.worker_tasks,
                    (unsigned long long)process_load.worker_busy_us, process_load.supervisor_avg_ms);
        std::printf("routes: local=%llu remote_emulated=%llu unavailable=%llu draining=%llu miss=%llu\n",
                    (unsigned long long)routes.local_delivered,
                    (unsigned long long)routes.remote_emulated,
                    (unsigned long long)routes.unavailable,
                    (unsigned long long)routes.draining,
                    (unsigned long long)routes.directory_miss);
        {
            // Simulation LOD distribution (current gauges) + actual update
            // rates (§25): how many entities really got AI/movement updates
            // per second. Work totals are cumulative (never reset); the
            // per-second windows in the diag log reset every second.
            std::uint64_t t_full = 0, t_reduced = 0, t_low = 0, t_dormant = 0;
            std::uint64_t x_full = 0, x_reduced = 0, x_low = 0, sleep_block = 0, wake_ext = 0;
            for (std::size_t zi = 0; zi < sim.Zones().ZoneCount(); ++zi) {
                const auto& diag = sim.Zones().GetZone(zi).Diagnostics();
                t_full += diag.lod_full.load(std::memory_order_relaxed);
                t_reduced += diag.lod_reduced.load(std::memory_order_relaxed);
                t_low += diag.lod_low.load(std::memory_order_relaxed);
                t_dormant += diag.lod_dormant.load(std::memory_order_relaxed);
                x_full += diag.cross_zone_full_since_diag.load(std::memory_order_relaxed);
                x_reduced += diag.cross_zone_reduced_since_diag.load(std::memory_order_relaxed);
                x_low += diag.cross_zone_low_since_diag.load(std::memory_order_relaxed);
                sleep_block += diag.sleep_blocked_external_since_diag.load(std::memory_order_relaxed);
                wake_ext += diag.wake_external_since_diag.load(std::memory_order_relaxed);
            }
            const auto work = sim.LodWorkTotalsSnapshot();
            const auto activity = sim.ActivityMetrics();
            const double elapsed = static_cast<double>(std::max(1, config.seconds));
            std::printf("lod tiers: full=%llu reduced=%llu low=%llu dormant=%llu\n",
                        (unsigned long long)t_full,
                        (unsigned long long)t_reduced,
                        (unsigned long long)t_low,
                        (unsigned long long)t_dormant);
            std::printf("lod work: ai_updates=%llu (%.0f/s) move_updates=%llu (%.0f/s) prom=%llu "
                        "dem=%llu wakes=%llu eval_us=%llu\n",
                        (unsigned long long)work.ai_updates,
                        work.ai_updates / elapsed,
                        (unsigned long long)work.move_updates,
                        work.move_updates / elapsed,
                        (unsigned long long)work.promotions,
                        (unsigned long long)work.demotions,
                        (unsigned long long)work.wakes,
                        (unsigned long long)work.eval_us);
            std::printf("activity: sources=%llu cells=%llu/%llu rebuilds=%llu rb_us=%llu "
                        "xzone=[%llu/%llu/%llu] sleep=[blocked=%llu wext=%llu]\n",
                        (unsigned long long)activity.sources,
                        (unsigned long long)activity.cells_nonempty,
                        (unsigned long long)activity.cells_total,
                        (unsigned long long)activity.rebuilds,
                        (unsigned long long)activity.rebuild_us_total,
                        (unsigned long long)x_full,
                        (unsigned long long)x_reduced,
                        (unsigned long long)x_low,
                        (unsigned long long)sleep_block,
                        (unsigned long long)wake_ext);
        }

        sim.Stop();
    }

    io.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }
    std::printf("BENCH-DONE validations=%d failures=%d\n", validations_run, validation_failures);
    return validation_failures == 0 ? 0 : 2;
}

int main(int argc, char** argv)
{
    try {
        return BenchMain(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "bench fatal: " << error.what() << "\n";
        return 1;
    }
}
