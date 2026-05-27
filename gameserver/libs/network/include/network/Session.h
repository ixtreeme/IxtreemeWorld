#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Types.h"

namespace gs::network {

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::ip::tcp::socket socket, gs::common::SessionId id);

    void Start();
    void Stop();

    [[nodiscard]] gs::common::SessionId Id() const { return id_; }

private:
    boost::asio::awaitable<void> ReadLoop();
    boost::asio::awaitable<void> WriteFrame(std::vector<std::uint8_t> payload);

    boost::asio::ip::tcp::socket socket_;
    gs::common::SessionId id_;
    bool stopped_ = false;
};

} // namespace gs::network
