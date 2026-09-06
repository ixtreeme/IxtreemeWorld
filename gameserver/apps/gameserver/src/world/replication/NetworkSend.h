#pragma once

#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "network/Session.h"

// Single templo for posting payloads back to IO sessions. Replication and
// the world runtime share it so socket-posting policy lives in one place.
namespace gs::game {

inline void SendToSession(boost::asio::io_context& io,
                          const std::shared_ptr<gs::network::Session>& session,
                          std::vector<std::uint8_t> payload)
{
    boost::asio::post(io, [session, payload = std::move(payload)]() mutable {
        session->SendPayload(std::move(payload));
    });
}

} // namespace gs::game
