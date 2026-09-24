#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include "common/Types.h"
#include "network/Session.h"

namespace gs::network {

// Runs `io` until it is stopped or runs out of work, like io_context::run(),
// but an exception escaping a handler does not end the calling io thread
// (hardening H8): it is logged, counted (io_loop_exceptions) and the loop
// resumes. The known sources are closed at their origin (a session closes
// on its own failures); this is the last line so one faulty handler cannot
// take an io worker -- on a std::thread, the whole process -- down.
void RunIoContext(boost::asio::io_context& io);

// Accept loop contract (hardening H2.3): a failure to accept or to serve ONE
// connection never ends the loop. Transient accept errors back off briefly
// and continue; a peer that vanished between accept and setup (e.g. an
// immediate RST) is dropped; only Stop() (acceptor closed) ends the loop.
// The acceptor lives on its own strand, so Stop() is safe from any thread.
class Server {
public:
    Server(boost::asio::io_context& io,
           std::uint16_t port,
           Session::PayloadHandler on_payload,
           Session::DisconnectHandler on_disconnect,
           std::function<void(std::shared_ptr<Session>)> on_connect = {});
    // Explicit bind endpoint (e.g. loopback-only for tests/tools, port 0 for
    // an ephemeral port). The port overload binds every IPv4 interface.
    Server(boost::asio::io_context& io,
           const boost::asio::ip::tcp::endpoint& endpoint,
           Session::PayloadHandler on_payload,
           Session::DisconnectHandler on_disconnect,
           std::function<void(std::shared_ptr<Session>)> on_connect = {});

    // Resource policy applied to every accepted session (call before Start).
    void SetSessionLimits(const SessionLimits& limits)
    {
        session_limits_ = limits;
    }

    void Start();
    void Stop();
    // Actually bound port (resolves an ephemeral port-0 bind).
    std::uint16_t LocalPort() const;

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t accept_errors = 0;     // transient, loop continued
        std::uint64_t dropped_on_setup = 0;  // peer gone / setup threw
    };
    Stats GetStats() const noexcept;

private:
    boost::asio::awaitable<void> AcceptLoop();

    boost::asio::io_context& io_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Session::PayloadHandler on_payload_;
    Session::DisconnectHandler on_disconnect_;
    std::function<void(std::shared_ptr<Session>)> on_connect_;
    SessionLimits session_limits_;
    gs::common::SessionId next_session_id_ = 1;
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> accept_errors_{0};
    std::atomic<std::uint64_t> dropped_on_setup_{0};
};

} // namespace gs::network
