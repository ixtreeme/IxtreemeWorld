#include "ConnectionHandler.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <kj/exception.h>

#include "common/Logging.h"
#include "protocol/Protocol.h"
#include "protocol/Serialization.h"

namespace gs::server {

ConnectionHandler::ConnectionHandler(AuthHandler& auth, GameHandler& game)
    : auth_(auth)
    , game_(game)
{
    auth_.SetAuthSuccessCallback(
        [this](auto session, AuthInfo auth) { OnAuthSuccess(session, auth); });
    auth_.SetAuthFailureCallback(
        [this](auto session, const std::string& reason) { OnAuthFailure(session, reason); });
}

void ConnectionHandler::OnPayload(std::shared_ptr<gs::network::Session> session,
                                  std::vector<std::uint8_t> payload)
{
    auto& ctx = contexts_[session->Id()];

    try {
        auto parsed = gs::protocol::ParsePacket(payload);
        if (!parsed) {
            LOG_WARN("Session {} sent malformed Cap'n Proto payload size: {}",
                     session->Id(),
                     payload.size());
            Disconnect(session, "invalid message");
            return;
        }

        auto packet = parsed->packet;

        switch (ctx.state) {
        case SessionState::WaitingHandshake:
            if (!packet.isHandshakeRequest()) {
                Disconnect(session, "wrong state");
                return;
            }
            HandleHandshakeRequest(session, ctx, packet.getHandshakeRequest());
            return;

        case SessionState::ConnectionEstablished:
            if (packet.isLoginRequest()) {
                ctx.state = SessionState::Authenticating;
                auth_.HandleLoginRequest(session, packet.getLoginRequest());
                return;
            }
            Disconnect(session, "wrong state");
            return;

        case SessionState::Authenticating:
            Disconnect(session, "packet during authentication");
            return;

        case SessionState::Authenticated:
            if (packet.isCharacterListRequest()) {
                game_.HandleCharacterListRequest(session, ctx.auth);
                return;
            }
            Disconnect(session, "unexpected packet type");
            return;
        }
    } catch (const kj::Exception& error) {
        LOG_WARN("Session {} sent invalid Cap'n Proto message: {}",
                 session->Id(),
                 error.getDescription().cStr());
        Disconnect(session, "invalid message");
    }
}

void ConnectionHandler::OnDisconnect(std::shared_ptr<gs::network::Session> session)
{
    contexts_.erase(session->Id());
    LOG_INFO("Session {} cleaned up", session->Id());
}

void ConnectionHandler::HandleHandshakeRequest(std::shared_ptr<gs::network::Session> session,
                                               SessionContext& ctx,
                                               gs::protocol::HandshakeRequest::Reader request)
{
    const auto client_version = request.getProtocolVersion();
    const auto client_build = request.getClientBuild();

    if (client_version != gs::protocol::kProtocolVersion) {
        SendHandshakeResponse(session,
                              gs::protocol::HandshakeResult::PROTOCOL_VERSION_MISMATCH,
                              "Protocol version mismatch",
                              true);
        LOG_INFO("Disconnecting session {}: protocol version mismatch", session->Id());
        return;
    }

    SendHandshakeResponse(session, gs::protocol::HandshakeResult::OK, "Welcome");
    ctx.state = SessionState::ConnectionEstablished;
    LOG_INFO("Session {} handshake OK (build {})", session->Id(), client_build.cStr());
}

void ConnectionHandler::SendHandshakeResponse(std::shared_ptr<gs::network::Session> session,
                                              gs::protocol::HandshakeResult result,
                                              const std::string& message,
                                              bool close_after_send)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto response = packet.initHandshakeResponse();
    response.setResult(result);
    response.setServerProtocolVersion(gs::protocol::kProtocolVersion);
    response.setMessage(message);

    if (close_after_send) {
        session->SendPayloadAndClose(gs::protocol::SerializeToBytes(msg));
    } else {
        session->SendPayload(gs::protocol::SerializeToBytes(msg));
    }
}

void ConnectionHandler::Disconnect(std::shared_ptr<gs::network::Session> session,
                                   const std::string& reason)
{
    LOG_INFO("Disconnecting session {}: {}", session->Id(), reason);
    session->Stop();
}

void ConnectionHandler::OnAuthSuccess(std::shared_ptr<gs::network::Session> session, AuthInfo auth)
{
    const auto it = contexts_.find(session->Id());
    if (it == contexts_.end()) {
        return;
    }

    auto& ctx = it->second;
    ctx.state = SessionState::Authenticated;
    ctx.auth = auth;
    LOG_INFO("Session {} authenticated: account_id={}",
             session->Id(),
             gs::db::ToUint64(auth.account_id));
}

void ConnectionHandler::OnAuthFailure(std::shared_ptr<gs::network::Session> session,
                                      const std::string& reason)
{
    LOG_INFO("Session {} auth failed: {}", session->Id(), reason);
}

} // namespace gs::server
