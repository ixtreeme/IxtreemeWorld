#include <csignal>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <sodium.h>

#include "common/Config.h"
#include "common/Logging.h"
#include "db/CharacterRepository.h"
#include "db/DbConfig.h"
#include "db/DbPool.h"
#include "db/HandoffTokenRepository.h"
#include "network/Server.h"

#include "GameConnectionHandler.h"
#include "world/WorldRuntime.h"

namespace {

struct AppOptions {
    std::filesystem::path config_path = "gameserver.conf";
    std::filesystem::path database_config_path = "database.json";
    // World selection overrides (MAP-1). CLI paths are relative to the
    // working directory at launch and made absolute immediately.
    std::optional<std::string> world_mode;
    std::optional<std::filesystem::path> world_package;
    std::optional<std::filesystem::path> mob_types_config;
    std::optional<std::filesystem::path> validate_package; // offline check, then exit
    bool startup_check = false; // full startup (world, DB, listen), then clean exit
};

[[noreturn]] void Usage()
{
    std::cerr << "Usage: gameserver [--config path] [--database-config path]\n"
                 "                  [--world-mode file|synthetic] [--world-package dir] [--mob-types path]\n"
                 "                  [--startup-check]\n"
                 "       gameserver --validate-world-package dir [--mob-types path] [--config path]\n";
    std::exit(1);
}

AppOptions ParseArgs(int argc, char* argv[])
{
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--config" && has_value) {
            options.config_path = argv[++i];
        } else if (arg == "--database-config" && has_value) {
            options.database_config_path = argv[++i];
        } else if (arg == "--world-mode" && has_value) {
            options.world_mode = argv[++i];
        } else if (arg == "--world-package" && has_value) {
            options.world_package = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--mob-types" && has_value) {
            options.mob_types_config = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--validate-world-package" && has_value) {
            options.validate_package = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--startup-check") {
            options.startup_check = true;
        } else {
            Usage();
        }
    }
    return options;
}

// ---- world selection (MAP-1) -------------------------------------------------
// Precedence: command line > config file > built-in default. Bases: a CLI
// path is relative to the working directory at launch; a config value is
// relative to the directory holding the config file (never to the cwd), so
// the same config finds the same package from any working directory.
// Defaults: world_mode = file; there is no default package or mob type path
// -- file mode without them is a startup error, not a fallback.
struct WorldPlan {
    bool synthetic = false;
    std::filesystem::path package;
    std::string package_source;
    std::filesystem::path mob_types;
    std::string mob_types_source;
    gs::game::WorldRuntime::SyntheticWorldConfig synthetic_config;
    std::string error;
};

std::filesystem::path ConfigRelative(const std::filesystem::path& config_dir, const std::string& value)
{
    const std::filesystem::path path(value);
    return (path.is_absolute() ? path : config_dir / path).lexically_normal();
}

// A key that is present but empty counts as not configured.
std::optional<std::string> NonEmpty(const gs::common::Config& config, const char* key)
{
    auto value = config.GetString(key);
    if (value && value->empty()) {
        value.reset();
    }
    return value;
}

WorldPlan ResolveWorldPlan(const AppOptions& options,
                           const gs::common::Config& config,
                           const std::filesystem::path& config_dir)
{
    WorldPlan plan;
    const std::string mode = options.world_mode.value_or(config.GetString("world_mode").value_or("file"));
    if (mode != "file" && mode != "synthetic") {
        plan.error = "world_mode '" + mode + "' is not 'file' or 'synthetic'";
        return plan;
    }
    plan.synthetic = mode == "synthetic";
    if (options.mob_types_config) {
        plan.mob_types = *options.mob_types_config;
        plan.mob_types_source = "cli";
    } else if (const auto value = NonEmpty(config, "mob_types_config")) {
        plan.mob_types = ConfigRelative(config_dir, *value);
        plan.mob_types_source = "config";
    }
    if (plan.synthetic) {
        const auto extent = config.GetString("synthetic_extent_m");
        plan.synthetic_config.extent_m = 4000.0f;
        if (extent) {
            try {
                plan.synthetic_config.extent_m = std::stof(*extent);
            } catch (const std::exception&) {
                plan.error = "synthetic_extent_m '" + *extent + "' is not a number";
                return plan;
            }
        }
        const int zx = config.GetInt("synthetic_zones_x").value_or(2);
        const int zy = config.GetInt("synthetic_zones_y").value_or(2);
        if (!(plan.synthetic_config.extent_m >= 500.0f && plan.synthetic_config.extent_m <= 1000000.0f) ||
            zx < 1 || zy < 1 || zx > 64 || zy > 64) {
            plan.error = "synthetic world needs 500 <= synthetic_extent_m <= 1000000 and 1..64 zones per axis";
            return plan;
        }
        plan.synthetic_config.zones_x = static_cast<std::uint32_t>(zx);
        plan.synthetic_config.zones_y = static_cast<std::uint32_t>(zy);
        plan.synthetic_config.mob_types_config = plan.mob_types.string();
        return plan;
    }
    if (options.world_package) {
        plan.package = *options.world_package;
        plan.package_source = "cli --world-package";
    } else if (const auto value = NonEmpty(config, "world_package")) {
        plan.package = ConfigRelative(config_dir, *value);
        plan.package_source = "config world_package";
    } else {
        plan.error = "world_mode=file but no world package is configured (set world_package in the config or "
                     "pass --world-package)";
        return plan;
    }
    if (plan.mob_types.empty()) {
        plan.error = "world_mode=file but no mob type registry is configured (set mob_types_config or pass "
                     "--mob-types)";
    }
    return plan;
}

void PrintPackageReport(const mx::map::PackageReport& report)
{
    for (const auto& issue : report.issues) {
        std::cout << "  " << issue.Format() << "\n";
    }
    const auto& m = report.manifest;
    if (m.format_version != 0) {
        std::cout << "  package: world_id=" << m.world_id << " format=v" << m.format_version
                  << " cells=" << m.size_cells_x << "x" << m.size_cells_y << " cell=" << m.cell_size_m
                  << "m chunks=" << m.chunk_grid_x << "x" << m.chunk_grid_y << " of " << m.chunk_size_cells
                  << " cells\n";
        for (const auto& layer : m.layers) {
            std::cout << "  layer " << mx::map::ToString(layer.kind) << ": " << mx::map::ToString(layer.status)
                      << (layer.required ? " required" : " optional") << " audience="
                      << mx::map::ToString(layer.audience) << (layer.implied ? " (v2 implied)" : "")
                      << (layer.file.empty() ? "" : " file=" + layer.file) << "\n";
        }
    }
    std::cout << "RESULT: " << (report.Ok() ? "VALID" : "INVALID")
              << " errors=" << report.Count(mx::map::IssueSeverity::Error)
              << " warnings=" << report.Count(mx::map::IssueSeverity::Warning)
              << " infos=" << report.Count(mx::map::IssueSeverity::Info) << " files=" << report.files_read
              << " bytes=" << report.bytes_read << " chunks=" << report.chunks_checked
              << " ms=" << report.elapsed_ms << "\n";
}

// One startup summary for the world actually running: identity, format,
// resolved path, bounds/origin/unit, chunk geometry, declared layers, load
// mode, memory + I/O, regions / leaf zones (read through a supervisor
// snapshot) and the real worker count.
void LogWorldStartupSummary(gs::game::WorldRuntime& sim,
                            const WorldPlan& plan,
                            const mx::map::PackageReport& report,
                            std::size_t spawn_points,
                            std::size_t mob_types)
{
    struct TopologyView {
        std::size_t regions = 0;
        std::size_t leaves = 0;
        std::size_t slots = 0;
        float extent_m = 0.0f;
    };
    auto future = sim.CaptureSnapshot<TopologyView>([](const gs::game::WorldRuntime::SnapshotContext& snap) {
        TopologyView view;
        view.regions = snap.zones.PartitionRoots().size();
        view.leaves = snap.zones.GetActiveLeaves().size();
        view.slots = snap.zones.ZoneCount();
        return view;
    });
    TopologyView topology;
    if (!sim.WaitSnapshot(future, std::chrono::seconds(10), topology)) {
        LOG_WARN("World startup summary: topology snapshot not served within 10s");
    }
    std::size_t workers = 0;
    for (int i = 0; i < 200 && workers == 0; ++i) {
        workers = sim.SchedulerStats().workers;
        if (workers == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (plan.synthetic) {
        LOG_WARN("World startup summary: mode=SYNTHETIC extent={}m x {}m origin=(0,0) terrain=flat (no package) "
                 "regions={} leaf_zones={} zone_slots={} workers={} -- benchmark/dev world",
                 plan.synthetic_config.extent_m,
                 plan.synthetic_config.extent_m,
                 topology.regions,
                 topology.leaves,
                 topology.slots,
                 workers);
        return;
    }
    const auto& m = report.manifest;
    std::string layers;
    for (const auto& layer : m.layers) {
        if (!layers.empty()) {
            layers += ", ";
        }
        layers += std::string(mx::map::ToString(layer.kind)) + ":" + mx::map::ToString(layer.status) + "/" +
                  mx::map::ToString(layer.audience) + (layer.required ? "/required" : "/optional");
    }
    LOG_INFO("World startup summary: mode=file world_id={} name='{}' format=v{} path={} bounds=(0,0)-({},{})m "
             "origin=({},{}) cell={}m height_unit=cm chunks={}x{} of {} cells layers=[{}] validation=startup",
             m.world_id,
             m.world_name,
             m.format_version,
             report.root.string(),
             m.ExtentX(),
             m.ExtentY(),
             m.origin_x,
             m.origin_y,
             m.cell_size_m,
             m.chunk_grid_x,
             m.chunk_grid_y,
             m.chunk_size_cells,
             layers);
    LOG_INFO("World startup summary: load=eager-resident files={} bytes={} chunks={} load_ms={:.1f} "
             "resident_terrain={}KB runtime_io=none (whole terrain resident; streaming + budgets arrive with "
             "MAP-3) spawn_points={} mob_types={} regions={} leaf_zones={} zone_slots={} workers={} "
             "issues: {} warning(s), {} info",
             report.files_read,
             report.bytes_read,
             report.chunks_checked,
             report.elapsed_ms,
             report.resident_terrain_bytes / 1024,
             spawn_points,
             mob_types,
             topology.regions,
             topology.leaves,
             topology.slots,
             workers,
             report.Count(mx::map::IssueSeverity::Warning),
             report.Count(mx::map::IssueSeverity::Info));
}

// Offline validator: every layer of the package at Full depth (plus the
// server's mob type cross-check when a registry is known). No DB, no network,
// no world runtime. Exit 0 = valid, 3 = invalid.
int RunValidateOnly(const AppOptions& options)
{
    gs::common::InitLogging("warn", {});
    gs::common::Config config;
    const bool config_loaded = config.Load(options.config_path);
    std::filesystem::path mob_types;
    if (options.mob_types_config) {
        mob_types = *options.mob_types_config;
    } else if (const auto value = config_loaded ? NonEmpty(config, "mob_types_config") : std::nullopt) {
        mob_types = ConfigRelative(std::filesystem::absolute(options.config_path).parent_path(), *value);
    }
    std::cout << "Validating world package " << options.validate_package->string() << " (depth=full"
              << (mob_types.empty() ? ", no mob type registry: spawn type cross-check skipped" : "") << ")\n";
    mx::map::PackageReport report;
    if (mob_types.empty()) {
        report = mx::map::ValidatePackage(*options.validate_package);
    } else {
        gs::game::WorldLoadRequest request{*options.validate_package, mob_types, mx::map::ValidationDepth::Full};
        (void)gs::game::LoadWorldPackage(request, report);
    }
    PrintPackageReport(report);
    return report.Ok() ? 0 : 3;
}

std::uint16_t ResolvePort(const gs::common::Config& config)
{
    constexpr int kDefaultPort = 11020;
    const auto port = config.GetInt("listen_port").value_or(kDefaultPort);
    if (port <= 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        return kDefaultPort;
    }
    return static_cast<std::uint16_t>(port);
}

std::uint32_t ResolveIoThreads(const gs::common::Config& config)
{
    const auto value = config.GetInt("io_threads").value_or(2);
    return static_cast<std::uint32_t>(std::clamp(value, 1, 4));
}

// Process topology identity for the distributed-ready runtime (§45).
// Defaults keep the single-process deployment: node=1/process=1, all
// configured zones local. A future multi-process deployment assigns
// distinct namespaces per process; no code changes needed, only config.
gs::game::RuntimeIdentity ResolveRuntimeIdentity(const gs::common::Config& config)
{
    const auto node = config.GetInt("node_id").value_or(1);
    const auto process = config.GetInt("process_id").value_or(1);
    gs::game::RuntimeIdentity identity;
    identity.node = gs::game::NodeId{static_cast<std::uint32_t>(std::max(0, node))};
    identity.process = gs::game::ProcessId{static_cast<std::uint32_t>(std::max(0, process))};
    return identity;
}

double GetDoubleOr(const gs::common::Config& config, const char* key, double fallback)
{
    const auto raw = config.GetString(key);
    if (!raw) {
        return fallback;
    }
    try {
        std::size_t used = 0;
        const double value = std::stod(*raw, &used);
        if (used == 0) {
            throw std::invalid_argument("no digits");
        }
        return value;
    } catch (const std::exception&) {
        LOG_WARN("Config key '{}' has non-numeric value '{}', using default {}", key, *raw, fallback);
        return fallback;
    }
}

// Dynamic partition configuration (§14). Every key is optional; invalid
// values warn + fall back per field (see ValidatePartitionConfig), and the
// effective set is logged by WorldRuntime::ConfigurePartition.
gs::game::PartitionConfig ResolvePartitionConfig(const gs::common::Config& config)
{
    gs::game::PartitionConfig out;
    out.split_load_threshold =
        static_cast<float>(GetDoubleOr(config, "partition_split_load_threshold", out.split_load_threshold));
    out.merge_load_threshold =
        static_cast<float>(GetDoubleOr(config, "partition_merge_load_threshold", out.merge_load_threshold));
    out.sustained_window_seconds =
        config.GetInt("partition_sustained_window_seconds").value_or(out.sustained_window_seconds);
    out.split_cooldown_seconds =
        config.GetInt("partition_split_cooldown_seconds").value_or(out.split_cooldown_seconds);
    out.merge_cooldown_seconds =
        config.GetInt("partition_merge_cooldown_seconds").value_or(out.merge_cooldown_seconds);
    out.tick_budget_ms =
        static_cast<float>(GetDoubleOr(config, "partition_tick_budget_ms", out.tick_budget_ms));
    out.resident_budget =
        static_cast<float>(GetDoubleOr(config, "partition_resident_budget", out.resident_budget));
    out.max_partition_depth =
        config.GetInt("partition_max_depth").value_or(out.max_partition_depth);
    out.min_zone_size_m =
        static_cast<float>(GetDoubleOr(config, "partition_min_zone_size_m", out.min_zone_size_m));

    // Adaptive split scoring (phase 2). min_zone_size_m is mirrored from the
    // effective partition floor by WorldRuntime::ConfigurePartition, never
    // read separately. All keys optional; invalid values warn + fall back.
    auto& scoring = out.scoring;
    scoring.min_expected_improvement = static_cast<float>(GetDoubleOr(
        config, "partition_min_expected_improvement", scoring.min_expected_improvement));
    scoring.boundary_band_m = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_boundary_band_m", scoring.boundary_band_m));
    scoring.hotspot_threshold = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_hotspot_threshold", scoring.hotspot_threshold));
    scoring.hotspot_max_count = static_cast<std::uint32_t>(std::max(
        0, config.GetInt("partition_scoring_hotspot_max_count")
               .value_or(static_cast<int>(scoring.hotspot_max_count))));
    scoring.weight_balance = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_weight_balance", scoring.weight_balance));
    scoring.weight_boundary = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_weight_boundary", scoring.weight_boundary));
    scoring.weight_migration = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_weight_migration", scoring.weight_migration));
    scoring.weight_replication = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_weight_replication", scoring.weight_replication));
    scoring.weight_instability = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_weight_instability", scoring.weight_instability));
    scoring.topology_penalty = static_cast<float>(
        GetDoubleOr(config, "partition_scoring_topology_penalty", scoring.topology_penalty));
    scoring.activity_band_budget = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_activity_band_budget", scoring.activity_band_budget));
    scoring.migration_band_budget = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_migration_band_budget", scoring.migration_band_budget));
    scoring.replication_band_budget = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_replication_band_budget", scoring.replication_band_budget));
    scoring.combat_band_budget = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_combat_band_budget", scoring.combat_band_budget));
    scoring.migration_work_budget = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_migration_work_budget", scoring.migration_work_budget));
    scoring.instability_window_s = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_instability_window_s", scoring.instability_window_s));
    scoring.why_not_log_seconds = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_why_not_log_seconds", scoring.why_not_log_seconds));
    const auto decision_log = config.GetString("partition_scoring_decision_log");
    if (decision_log &&
        (*decision_log == "0" || *decision_log == "false" || *decision_log == "off")) {
        scoring.decision_log_enabled = false;
    }
    const auto timescale = config.GetString("partition_scoring_timescale");
    if (timescale) {
        if (*timescale == "current") {
            scoring.decision_timescale = gs::game::LoadTimescale::Current;
        } else if (*timescale == "slow") {
            scoring.decision_timescale = gs::game::LoadTimescale::Slow;
        } else if (*timescale == "predicted") {
            scoring.decision_timescale = gs::game::LoadTimescale::Predicted;
        } else if (*timescale == "fast") {
            scoring.decision_timescale = gs::game::LoadTimescale::Fast;
        } else {
            LOG_WARN("Config key 'partition_scoring_timescale' has unknown value '{}', using fast",
                     *timescale);
        }
    }

    // Phase 3: adaptive merge scoring + partition stability controller. All
    // keys optional; invalid values warn + fall back (threshold invariant:
    // merge threshold < split threshold - post-merge safety margin).
    scoring.merge_sustained_low_s = static_cast<float>(
        GetDoubleOr(config, "partition_merge_sustained_low_seconds", scoring.merge_sustained_low_s));
    scoring.split_to_merge_cooldown_s = static_cast<float>(GetDoubleOr(
        config, "partition_split_to_merge_cooldown_seconds", scoring.split_to_merge_cooldown_s));
    scoring.merge_to_split_cooldown_s = static_cast<float>(GetDoubleOr(
        config, "partition_merge_to_split_cooldown_seconds", scoring.merge_to_split_cooldown_s));
    scoring.post_merge_safety_margin = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_post_merge_safety_margin", scoring.post_merge_safety_margin));
    scoring.min_merge_improvement = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_min_merge_improvement", scoring.min_merge_improvement));
    scoring.merge_topology_benefit = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_merge_topology_benefit", scoring.merge_topology_benefit));
    scoring.weight_merge_topology = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_topology", scoring.weight_merge_topology));
    scoring.weight_merge_boundary = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_boundary", scoring.weight_merge_boundary));
    scoring.weight_merge_migration = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_migration", scoring.weight_merge_migration));
    scoring.weight_merge_replication = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_replication", scoring.weight_merge_replication));
    scoring.weight_merge_risk = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_risk", scoring.weight_merge_risk));
    scoring.weight_merge_execution = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_execution", scoring.weight_merge_execution));
    scoring.weight_merge_instability = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_weight_merge_instability", scoring.weight_merge_instability));
    scoring.oscillation_window_s = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_oscillation_window_seconds", scoring.oscillation_window_s));
    scoring.emergency_p99_multiplier = static_cast<float>(GetDoubleOr(
        config, "partition_scoring_emergency_p99_multiplier", scoring.emergency_p99_multiplier));
    const auto emergency_bypass = config.GetString("partition_scoring_emergency_split_bypass");
    if (emergency_bypass &&
        (*emergency_bypass == "0" || *emergency_bypass == "false" || *emergency_bypass == "off")) {
        scoring.emergency_split_bypass = false;
    }
    // Adaptive control master switch (observe-only baseline when off).
    const auto adaptive = config.GetString("partition_scoring_adaptive_enabled");
    if (adaptive && (*adaptive == "0" || *adaptive == "false" || *adaptive == "off")) {
        scoring.adaptive_enabled = false;
    }
    return out;
}

// Simulation LOD configuration. Every key is optional; invalid values warn
// + fall back per field (see ValidateLodConfig). The effective set is logged
// by WorldRuntime::ConfigureSimulationLod.
gs::game::LodConfig ResolveLodConfig(const gs::common::Config& config)
{
    gs::game::LodConfig out;
    const auto enabled = config.GetString("simulation_lod_enabled");
    if (enabled && (*enabled == "0" || *enabled == "false" || *enabled == "off")) {
        out.enabled = false;
    }
    out.full_radius_m =
        static_cast<float>(GetDoubleOr(config, "simulation_full_radius_m", out.full_radius_m));
    out.reduced_radius_m =
        static_cast<float>(GetDoubleOr(config, "simulation_reduced_radius_m", out.reduced_radius_m));
    out.low_radius_m =
        static_cast<float>(GetDoubleOr(config, "simulation_low_radius_m", out.low_radius_m));
    out.reduced_hz =
        static_cast<float>(GetDoubleOr(config, "simulation_reduced_hz", out.reduced_hz));
    out.low_hz = static_cast<float>(GetDoubleOr(config, "simulation_low_hz", out.low_hz));
    out.demote_full_sec =
        static_cast<float>(GetDoubleOr(config, "simulation_demote_full_sec", out.demote_full_sec));
    out.demote_reduced_sec = static_cast<float>(
        GetDoubleOr(config, "simulation_demote_reduced_sec", out.demote_reduced_sec));
    out.demote_low_sec =
        static_cast<float>(GetDoubleOr(config, "simulation_demote_low_sec", out.demote_low_sec));
    return out;
}

// Continuous load field configuration (Adaptive Simulation Fabric phase 1).
// Every key is optional; invalid values warn + fall back per field (see
// ValidateLoadFieldConfig), and the effective set is logged by
// WorldRuntime::ConfigureLoadField. World bounds are runtime-owned.
gs::game::LoadFieldConfig ResolveLoadFieldConfig(const gs::common::Config& config)
{
    gs::game::LoadFieldConfig out;
    const auto enabled = config.GetString("load_field_enabled");
    if (enabled && (*enabled == "0" || *enabled == "false" || *enabled == "off")) {
        out.enabled = false;
    }
    out.cell_size_m =
        static_cast<float>(GetDoubleOr(config, "load_field_cell_size_m", out.cell_size_m));
    out.aggregation_hz =
        static_cast<float>(GetDoubleOr(config, "load_field_aggregation_hz", out.aggregation_hz));
    const auto l1_enabled = config.GetString("load_field_l1_enabled");
    if (l1_enabled && (*l1_enabled == "0" || *l1_enabled == "false" || *l1_enabled == "off")) {
        out.l1_enabled = false;
    }
    const auto l1_ratio = config.GetInt("load_field_l1_ratio").value_or(
        static_cast<int>(out.l1_ratio));
    out.l1_ratio = static_cast<std::uint32_t>(std::max(0, l1_ratio));
    out.simulation_budget =
        static_cast<float>(GetDoubleOr(config, "load_field_simulation_budget", out.simulation_budget));
    out.replication_budget = static_cast<float>(
        GetDoubleOr(config, "load_field_replication_budget", out.replication_budget));
    out.aoi_budget =
        static_cast<float>(GetDoubleOr(config, "load_field_aoi_budget", out.aoi_budget));
    out.combat_budget =
        static_cast<float>(GetDoubleOr(config, "load_field_combat_budget", out.combat_budget));
    out.migration_budget = static_cast<float>(
        GetDoubleOr(config, "load_field_migration_budget", out.migration_budget));
    out.weight_simulation = static_cast<float>(
        GetDoubleOr(config, "load_field_weight_simulation", out.weight_simulation));
    out.weight_replication = static_cast<float>(
        GetDoubleOr(config, "load_field_weight_replication", out.weight_replication));
    out.weight_aoi = static_cast<float>(GetDoubleOr(config, "load_field_weight_aoi", out.weight_aoi));
    out.weight_combat = static_cast<float>(
        GetDoubleOr(config, "load_field_weight_combat", out.weight_combat));
    out.weight_migration = static_cast<float>(
        GetDoubleOr(config, "load_field_weight_migration", out.weight_migration));
    out.fast_rise_tau_s = static_cast<float>(
        GetDoubleOr(config, "load_field_fast_rise_tau_s", out.fast_rise_tau_s));
    out.fast_fall_tau_s = static_cast<float>(
        GetDoubleOr(config, "load_field_fast_fall_tau_s", out.fast_fall_tau_s));
    out.slow_rise_tau_s = static_cast<float>(
        GetDoubleOr(config, "load_field_slow_rise_tau_s", out.slow_rise_tau_s));
    out.slow_fall_tau_s = static_cast<float>(
        GetDoubleOr(config, "load_field_slow_fall_tau_s", out.slow_fall_tau_s));
    return out;
}

// Network resource policy (hardening H3). Every key optional; negative values
// fall back to the default, 0 disables that limit.
gs::network::SessionLimits ResolveSessionLimits(const gs::common::Config& config)
{
    gs::network::SessionLimits limits;
    auto get = [&](const char* key, long long fallback) {
        const auto value = config.GetInt(key);
        return value && *value >= 0 ? static_cast<long long>(*value) : fallback;
    };
    limits.send_soft_bytes = static_cast<std::size_t>(get("net_send_queue_soft_kb", 512)) * 1024;
    limits.send_hard_bytes = static_cast<std::size_t>(get("net_send_queue_hard_kb", 8192)) * 1024;
    limits.send_hard_frames = static_cast<std::size_t>(get("net_send_queue_hard_frames", 20000));
    limits.send_hard_age = std::chrono::milliseconds(get("net_send_queue_hard_age_ms", 10000));
    limits.idle_timeout = std::chrono::milliseconds(get("net_idle_timeout_ms", 60000));
    limits.setup_timeout = std::chrono::milliseconds(get("net_handshake_timeout_ms", 10000));
    return limits;
}

gs::game::ConnectionPolicy ResolveConnectionPolicy(const gs::common::Config& config)
{
    gs::game::ConnectionPolicy policy;
    const auto max_moves = config.GetInt("input_max_move_packets_per_second");
    if (max_moves && *max_moves >= 0) {
        policy.max_move_packets_per_second = static_cast<std::uint32_t>(*max_moves);
    }
    policy.attacks_per_second = static_cast<float>(
        GetDoubleOr(config, "input_attacks_per_second", policy.attacks_per_second));
    policy.attack_burst =
        static_cast<float>(GetDoubleOr(config, "input_attack_burst", policy.attack_burst));
    return policy;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = ParseArgs(argc, argv);
        if (options.validate_package) {
            return RunValidateOnly(options);
        }

        gs::common::Config config;
        const bool loaded = config.Load(options.config_path);
        const auto config_dir = std::filesystem::absolute(options.config_path).parent_path();

        const auto port = ResolvePort(config);
        const auto io_threads = ResolveIoThreads(config);
        const auto game_server =
            config.GetString("game_server").value_or("127.0.0.1:" + std::to_string(port));
        const auto log_level = config.GetString("log_level").value_or("info");
        const auto log_file = config.GetString("log_file").value_or("logs/gameserver.log");

        gs::common::InitLogging(log_level, log_file);
        if (!loaded) {
            LOG_WARN("Config file '{}' not found, using defaults", options.config_path.string());
        }

        // ---- World first (MAP-1): the package is loaded and validated before
        // anything with side effects exists (no DB connection, no listener, no
        // runtime). A rejected package ends the process here -- there is no
        // flat/fallback world.
        const WorldPlan plan = ResolveWorldPlan(options, config, config_dir);
        if (!plan.error.empty()) {
            LOG_ERROR("World configuration invalid: {}", plan.error);
            std::cerr << "Fatal: world configuration invalid: " << plan.error << '\n';
            return 2;
        }
        std::optional<gs::game::LoadedWorld> loaded_world;
        if (!plan.synthetic) {
            LOG_INFO("World package: mode=file path={} (from {}) mob_types={} (from {})",
                     plan.package.string(),
                     plan.package_source,
                     plan.mob_types.string(),
                     plan.mob_types_source);
            mx::map::PackageReport report;
            loaded_world = gs::game::LoadWorldPackage(
                gs::game::WorldLoadRequest{plan.package, plan.mob_types, mx::map::ValidationDepth::Startup},
                report);
            gs::game::LogPackageReport(report);
            if (!loaded_world) {
                const auto* first = report.FirstError();
                LOG_ERROR("World package rejected ({} error(s)); refusing to start. No fallback world.",
                          report.Count(mx::map::IssueSeverity::Error));
                std::cerr << "Fatal: world package " << plan.package.string() << " rejected ("
                          << report.Count(mx::map::IssueSeverity::Error) << " error(s)); first: "
                          << (first != nullptr ? first->Format() : std::string("?")) << '\n';
                return 3;
            }
        } else {
            LOG_WARN("World mode SYNTHETIC (explicit): flat {}m x {}m world, {}x{} bootstrap zones, mob_types={} "
                     "-- benchmark/dev only, NOT a real map",
                     plan.synthetic_config.extent_m,
                     plan.synthetic_config.extent_m,
                     plan.synthetic_config.zones_x,
                     plan.synthetic_config.zones_y,
                     plan.mob_types.empty() ? std::string("none") : plan.mob_types.string());
        }

        if (sodium_init() < 0) {
            LOG_ERROR("libsodium initialization failed");
            return 1;
        }

        auto db_config = gs::db::LoadDbConfig(options.database_config_path);

        LOG_INFO("GameServer starting on port {}", port);
        boost::asio::io_context io;
        gs::db::DbPool db_pool(io, std::move(db_config));
        db_pool.Start();

        gs::db::CharacterRepository characters(db_pool);
        gs::db::HandoffTokenRepository handoff_tokens(db_pool);
        const auto runtime_identity = ResolveRuntimeIdentity(config);
        LOG_INFO("Runtime identity: node={} process={} (namespace {})",
                 runtime_identity.node.value,
                 runtime_identity.process.value,
                 gs::game::NamespaceFor(runtime_identity));
        mx::map::PackageReport world_report;
        std::size_t spawn_point_count = 0;
        std::size_t mob_type_count = 0;
        if (loaded_world) {
            world_report = loaded_world->report;
            spawn_point_count = loaded_world->spawn_points.size();
            mob_type_count = loaded_world->mob_type_count;
        }
        auto sim_ptr = loaded_world
                           ? std::make_unique<gs::game::WorldRuntime>(io, runtime_identity, std::move(*loaded_world))
                           : std::make_unique<gs::game::WorldRuntime>(io, runtime_identity, plan.synthetic_config);
        loaded_world.reset();
        gs::game::WorldRuntime& sim = *sim_ptr;
        // Zone worker threads: 0/absent = hardware_concurrency - 1 (H6: never
        // derived from the seed zone count; splits add zones at runtime).
        if (const auto zone_workers = config.GetInt("zone_workers"); zone_workers && *zone_workers > 0) {
            sim.ConfigureWorkers(static_cast<std::size_t>(*zone_workers));
        }
        sim.ConfigurePartition(ResolvePartitionConfig(config));
        sim.ConfigureSimulationLod(ResolveLodConfig(config));
        sim.ConfigureLoadField(ResolveLoadFieldConfig(config));
        sim.Start();
        LogWorldStartupSummary(sim, plan, world_report, spawn_point_count, mob_type_count);

        const auto session_limits = ResolveSessionLimits(config);
        const auto connection_policy = ResolveConnectionPolicy(config);
        LOG_INFO("Network policy: send_queue soft={}KB hard={}KB/{} frames/{}ms idle={}ms "
                 "handshake={}ms move_packets_max={}/s attacks={}/s burst={}",
                 session_limits.send_soft_bytes / 1024,
                 session_limits.send_hard_bytes / 1024,
                 session_limits.send_hard_frames,
                 session_limits.send_hard_age.count(),
                 session_limits.idle_timeout.count(),
                 session_limits.setup_timeout.count(),
                 connection_policy.max_move_packets_per_second,
                 connection_policy.attacks_per_second,
                 connection_policy.attack_burst);
        gs::game::GameConnectionHandler handler(handoff_tokens, characters, sim, game_server,
                                                connection_policy);
        gs::network::Server server(
            io,
            port,
            [&handler](auto session, auto payload) {
                handler.OnPayload(session, std::move(payload));
            },
            [&handler](auto session) {
                handler.OnDisconnect(session);
            },
            [](auto session) {
                LOG_INFO("GameServer New connection: session id {}", session->Id());
            });

        server.SetSessionLimits(session_limits);

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                LOG_INFO("Received signal {}, stopping game server", signal_number);
                server.Stop();
                io.stop();
            }
        });

        server.Start();

        if (options.startup_check) {
            // Full startup reached (validated world running, DB connected,
            // listener bound): stop in order instead of serving.
            LOG_INFO("Startup check passed: world running, database connected, listening on port {}; "
                     "shutting down",
                     port);
            std::cout << "STARTUP-CHECK OK port=" << port << '\n';
            server.Stop();
            sim.Stop();
            db_pool.Stop();
            return 0;
        }

        std::vector<std::thread> io_workers;
        io_workers.reserve(io_threads > 0 ? io_threads - 1 : 0);
        for (std::uint32_t i = 1; i < io_threads; ++i) {
            io_workers.emplace_back([&io] {
                gs::network::RunIoContext(io);
            });
        }
        gs::network::RunIoContext(io); // H8: a throwing handler does not end an io thread

        for (auto& worker : io_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        sim.Stop();
        db_pool.Stop();
        LOG_INFO("GameServer shutting down cleanly");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
