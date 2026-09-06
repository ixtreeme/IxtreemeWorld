#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/CharacterRepository.h"
#include "db/HandoffTokenRepository.h"
#include "network/Session.h"
#include "schema/packet.capnp.h"

#include "world/WorldRuntime.h"

namespace gs::game {

enum class GameSessionState {
    WaitingHandshake,
    ConnectionEstablished,
    EnteringWorld,
    InWorld,
};

struct GameSessionContext {
    GameSessionState state = GameSessionState::WaitingHandshake;
};

class GameConnectionHandler {
public:
    GameConnectionHandler(gs::db::HandoffTokenRepository& handoff_tokens,
                           gs::db::CharacterRepository& characters,
                           WorldRuntime& sim,
                           std::string game_server);

    void OnPayload(std::shared_ptr<gs::network::Session> session,
                   std::vector<std::uint8_t> payload);
    void OnDisconnect(std::shared_ptr<gs::network::Session> session);

private:
    void HandleHandshakeRequest(std::shared_ptr<gs::network::Session> session,
                                GameSessionContext& ctx,
                                gs::protocol::HandshakeRequest::Reader request);
    void HandleEnterWorld(std::shared_ptr<gs::network::Session> session,
                          GameSessionContext& ctx,
                          gs::protocol::C2sEnterWorld::Reader request);
    void SendHandshakeResponse(std::shared_ptr<gs::network::Session> session,
                               gs::protocol::HandshakeResult result,
                               const std::string& message,
                               bool close_after_send = false);
    void SendEnterWorldReject(std::shared_ptr<gs::network::Session> session,
                              gs::protocol::S2cEnterWorldReject::RejectReason reason,
                              bool close_after_send = true);
    void Disconnect(std::shared_ptr<gs::network::Session> session, const std::string& reason);

    gs::db::HandoffTokenRepository& handoff_tokens_;
    gs::db::CharacterRepository& characters_;
    WorldRuntime& sim_;
    std::string game_server_;
    std::mutex contexts_mutex_;
    std::unordered_map<gs::common::SessionId, GameSessionContext> contexts_;
};

} // namespace gs::game
