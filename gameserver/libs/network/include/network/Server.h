#pragma once

#include <cstdint>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Types.h"

namespace gs::network {

class Server {
public:
    Server(boost::asio::io_context& io, std::uint16_t port);

    void Start();
    void Stop();

private:
    boost::asio::awaitable<void> AcceptLoop();

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    gs::common::SessionId next_session_id_ = 1;
};

} // namespace gs::network
