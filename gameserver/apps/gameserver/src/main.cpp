#include <csignal>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
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
};

AppOptions ParseArgs(int argc, char* argv[])
{
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
        } else if (arg == "--database-config" && i + 1 < argc) {
            options.database_config_path = argv[++i];
        } else {
            std::cerr << "Usage: gameserver [--config path] [--database-config path]\n";
            std::exit(1);
        }
    }
    return options;
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

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = ParseArgs(argc, argv);

        gs::common::Config config;
        const bool loaded = config.Load(options.config_path);

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
        gs::game::WorldRuntime sim(io, runtime_identity);
        sim.ConfigurePartition(ResolvePartitionConfig(config));
        sim.ConfigureSimulationLod(ResolveLodConfig(config));
        sim.ConfigureLoadField(ResolveLoadFieldConfig(config));
        sim.Start();

        gs::game::GameConnectionHandler handler(handoff_tokens, characters, sim, game_server);
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

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                LOG_INFO("Received signal {}, stopping game server", signal_number);
                server.Stop();
                io.stop();
            }
        });

        server.Start();

        std::vector<std::thread> io_workers;
        io_workers.reserve(io_threads > 0 ? io_threads - 1 : 0);
        for (std::uint32_t i = 1; i < io_threads; ++i) {
            io_workers.emplace_back([&io] {
                io.run();
            });
        }
        io.run();

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
