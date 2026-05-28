#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "common/Types.h"

namespace gs::network {

class Session : public std::enable_shared_from_this<Session> {
public:
    using PayloadHandler = std::function<void(std::shared_ptr<Session> session,
                                              std::vector<std::uint8_t> payload)>;
    using DisconnectHandler = std::function<void(std::shared_ptr<Session> session)>;

    explicit Session(boost::asio::ip::tcp::socket socket, gs::common::SessionId id);

    void Start(PayloadHandler on_payload, DisconnectHandler on_disconnect);
    void Stop();
    void SendPayload(std::vector<std::uint8_t> payload);
    void SendPayloadAndClose(std::vector<std::uint8_t> payload);

    [[nodiscard]] gs::common::SessionId Id() const { return id_; }

private:
    boost::asio::awaitable<void> ReadLoop();
    boost::asio::awaitable<void> WriteFrame(std::vector<std::uint8_t> payload);
    void SendPayloadInternal(std::vector<std::uint8_t> payload, bool close_after_send);

    boost::asio::ip::tcp::socket socket_;
    gs::common::SessionId id_;
    PayloadHandler on_payload_;
    DisconnectHandler on_disconnect_;
    bool stopped_ = false;
    bool disconnect_notified_ = false;
};

} // namespace gs::network
