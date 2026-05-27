#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "common/Config.h"
#include "common/Logging.h"
#include "network/Server.h"

namespace {

struct AppOptions {
    std::filesystem::path config_path = "gameserver.conf";
};

AppOptions ParseArgs(int argc, char* argv[])
{
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
        } else {
            std::cerr << "Usage: gameserver [--config path]\n";
            std::exit(1);
        }
    }
    return options;
}

std::uint16_t ResolvePort(const gs::common::Config& config)
{
    constexpr int kDefaultPort = 11000;
    const auto port = config.GetInt("listen_port").value_or(kDefaultPort);
    if (port <= 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        return kDefaultPort;
    }
    return static_cast<std::uint16_t>(port);
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = ParseArgs(argc, argv);

        gs::common::Config config;
        const bool loaded = config.Load(options.config_path);

        const auto port = ResolvePort(config);
        const auto log_level = config.GetString("log_level").value_or("info");
        const auto log_file = config.GetString("log_file").value_or("logs/gameserver.log");

        gs::common::InitLogging(log_level, log_file);
        if (!loaded) {
            LOG_WARN("Config file '{}' not found, using defaults", options.config_path.string());
        }

        LOG_INFO("GameServer starting on port {}", port);

        boost::asio::io_context io;
        gs::network::Server server(io, port);

        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                LOG_INFO("Received signal {}, stopping server", signal_number);
                server.Stop();
                io.stop();
            }
        });

        server.Start();
        io.run();

        LOG_INFO("GameServer shutting down cleanly");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
