#include "network/Server.h"

#include <exception>
#include <memory>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "common/Logging.h"
#include "network/Session.h"

namespace gs::network {
namespace asio = boost::asio;
using boost::asio::ip::tcp;

Server::Server(asio::io_context& io, std::uint16_t port)
    : io_(io)
    , acceptor_(io, tcp::endpoint(tcp::v4(), port))
{
}

void Server::Start()
{
    asio::co_spawn(
        io_,
        [this]() -> asio::awaitable<void> {
            co_await AcceptLoop();
        },
        asio::detached);
}

void Server::Stop()
{
    boost::system::error_code ignored;
    acceptor_.close(ignored);
}

asio::awaitable<void> Server::AcceptLoop()
{
    try {
        for (;;) {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            const auto remote = socket.remote_endpoint();
            const auto session_id = next_session_id_++;

            LOG_INFO("New connection from {}:{}, session id {}",
                     remote.address().to_string(),
                     remote.port(),
                     session_id);

            auto session = std::make_shared<Session>(std::move(socket), session_id);
            session->Start();
        }
    } catch (const boost::system::system_error& error) {
        if (acceptor_.is_open()) {
            LOG_ERROR("Accept loop failed: {}", error.code().message());
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Accept loop error: {}", error.what());
    }
}

} // namespace gs::network
