#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <network/Session.h>

#include "AuthHandler.h"
#include "common/Types.h"
#include "GameHandler.h"
#include "SessionContext.h"
#include "schema/packet.capnp.h"

namespace gs::server {

class ConnectionHandler {
public:
    ConnectionHandler(AuthHandler& auth, GameHandler& game);

    void OnPayload(std::shared_ptr<gs::network::Session> session,
                   std::vector<std::uint8_t> payload);
    void OnDisconnect(std::shared_ptr<gs::network::Session> session);

private:
    void HandleHandshakeRequest(std::shared_ptr<gs::network::Session> session,
                                SessionContext& ctx,
                                gs::protocol::HandshakeRequest::Reader request);

    void SendHandshakeResponse(std::shared_ptr<gs::network::Session> session,
                               gs::protocol::HandshakeResult result,
                               const std::string& message,
                               bool close_after_send = false);

    void Disconnect(std::shared_ptr<gs::network::Session> session, const std::string& reason);

    void OnAuthSuccess(std::shared_ptr<gs::network::Session> session, AuthInfo auth);
    void OnAuthFailure(std::shared_ptr<gs::network::Session> session,
                       const std::string& reason);

    AuthHandler& auth_;
    GameHandler& game_;
    std::unordered_map<gs::common::SessionId, SessionContext> contexts_;
};

} // namespace gs::server
