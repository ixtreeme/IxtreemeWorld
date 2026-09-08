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
