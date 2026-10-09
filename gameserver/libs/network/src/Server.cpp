#include "network/Server.h"

#include <chrono>
#include <exception>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "common/Logging.h"

namespace gs::network {
namespace asio = boost::asio;
using boost::asio::ip::tcp;

void RunIoContext(asio::io_context& io)
{
    for (;;) {
        try {
            io.run();
            return; // stopped, or out of work
        } catch (const std::exception& error) {
            GlobalSessionCounters().io_loop_exceptions.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("io handler threw, io loop resumes: {}", error.what());
        } catch (...) {
            GlobalSessionCounters().io_loop_exceptions.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("io handler threw a non-standard exception, io loop resumes");
        }
    }
}

Server::Server(asio::io_context& io,
               std::uint16_t port,
               Session::PayloadHandler on_payload,
               Session::DisconnectHandler on_disconnect,
               std::function<void(std::shared_ptr<Session>)> on_connect)
    : Server(io,
             tcp::endpoint(tcp::v4(), port),
             std::move(on_payload),
             std::move(on_disconnect),
             std::move(on_connect))
{
}

Server::Server(asio::io_context& io,
               const tcp::endpoint& endpoint,
               Session::PayloadHandler on_payload,
               Session::DisconnectHandler on_disconnect,
               std::function<void(std::shared_ptr<Session>)> on_connect)
    : io_(io)
    , strand_(asio::make_strand(io))
    , acceptor_(strand_, endpoint)
    , on_payload_(std::move(on_payload))
    , on_disconnect_(std::move(on_disconnect))
    , on_connect_(std::move(on_connect))
{
}

std::uint16_t Server::LocalPort() const
{
    boost::system::error_code ec;
    const auto endpoint = acceptor_.local_endpoint(ec);
    return ec ? 0 : endpoint.port();
}

Server::Stats Server::GetStats() const noexcept
{
    return Stats{accepted_.load(std::memory_order_relaxed),
                 accept_errors_.load(std::memory_order_relaxed),
                 dropped_on_setup_.load(std::memory_order_relaxed)};
}

void Server::Start()
{
    asio::co_spawn(
        strand_,
        [this]() -> asio::awaitable<void> {
            co_await AcceptLoop();
        },
        asio::detached);
}

void Server::Stop()
{
    asio::dispatch(strand_, [this] {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
    });
}

asio::awaitable<void> Server::AcceptLoop()
{
    for (;;) {
        boost::system::error_code ec;
        // Sessions get the plain io executor; each Session serializes itself
        // on its own strand.
        auto socket = co_await acceptor_.async_accept(io_, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec == asio::error::operation_aborted || !acceptor_.is_open()) {
                break; // Stop()
            }
            // Transient (ECONNABORTED, EMFILE/ENFILE, ENOBUFS, ...): never
            // fatal for the listener. A short back-off keeps a resource
            // exhaustion from turning into a hot loop.
            accept_errors_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("Accept failed (listener continues): {}", ec.message());
            asio::steady_timer backoff(strand_, std::chrono::milliseconds(50));
            boost::system::error_code timer_ec;
            co_await backoff.async_wait(asio::redirect_error(asio::use_awaitable, timer_ec));
            continue;
        }

        boost::system::error_code endpoint_ec;
        const auto remote = socket.remote_endpoint(endpoint_ec);
        if (endpoint_ec) {
            // The peer vanished between accept and here (e.g. connect+RST):
            // nothing to serve, and nothing to kill the listener over.
            dropped_on_setup_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG("Dropped connection before setup: {}", endpoint_ec.message());
            continue;
        }

        // Hardening H10: a real-time protocol of small frames (20 Hz
        // replication, input) must not wait for Nagle to coalesce them
        // behind an unacknowledged frame (with a delayed ACK: 40-200 ms).
        boost::system::error_code nodelay_ec;
        socket.set_option(tcp::no_delay(true), nodelay_ec);
        if (nodelay_ec) {
            LOG_DEBUG("TCP_NODELAY not applied: {}", nodelay_ec.message());
        }

        const auto session_id = next_session_id_++;
        LOG_INFO("New connection from {}:{}, session id {}",
                 remote.address().to_string(),
                 remote.port(),
                 session_id);
        try {
            auto session = std::make_shared<Session>(std::move(socket), session_id, session_limits_);
            if (on_connect_) {
                on_connect_(session);
            }
            session->Start(on_payload_, on_disconnect_);
            accepted_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& error) {
            dropped_on_setup_.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("Session {} setup failed (listener continues): {}", session_id, error.what());
        }
    }
}

} // namespace gs::network
