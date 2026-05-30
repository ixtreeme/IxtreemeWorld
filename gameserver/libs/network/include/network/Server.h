#pragma once

#include <cstdint>
#include <functional>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Types.h"
#include "network/Session.h"

namespace gs::network {

class Server {
public:
    Server(boost::asio::io_context& io,
           std::uint16_t port,
           Session::PayloadHandler on_payload,
           Session::DisconnectHandler on_disconnect,
           std::function<void(std::shared_ptr<Session>)> on_connect = {});

    void Start();
    void Stop();

private:
    boost::asio::awaitable<void> AcceptLoop();

    boost::asio::io_context& io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    Session::PayloadHandler on_payload_;
    Session::DisconnectHandler on_disconnect_;
    std::function<void(std::shared_ptr<Session>)> on_connect_;
    gs::common::SessionId next_session_id_ = 1;
};

} // namespace gs::network
