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
        gs::game::WorldRuntime sim(io);
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
