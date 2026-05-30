#pragma once

#include <memory>
#include <string>
#include <vector>

#include <db/CharacterRepository.h>
#include <db/HandoffTokenRepository.h>
#include <network/Session.h>

#include "SessionContext.h"
#include "schema/packet.capnp.h"

namespace gs::server {

class GameHandler {
public:
    GameHandler(gs::db::CharacterRepository& characters,
                gs::db::HandoffTokenRepository& handoff_tokens,
                std::string game_host,
                std::uint16_t game_port,
                std::string game_server);

    void HandleCharacterListRequest(std::shared_ptr<gs::network::Session> session,
                                    const AuthInfo& auth);
    void HandleCharacterSelect(std::shared_ptr<gs::network::Session> session,
                               const AuthInfo& auth,
                               gs::protocol::C2sCharacterSelect::Reader request);

private:
    void SendCharacterListResponse(std::shared_ptr<gs::network::Session> session,
                                   gs::protocol::CharacterListResult result,
                                   const std::string& message,
                                   const std::vector<gs::db::Character>& characters);

    void SendEnterWorldToken(std::shared_ptr<gs::network::Session> session,
                             const std::string& token);

    gs::db::CharacterRepository& characters_;
    gs::db::HandoffTokenRepository& handoff_tokens_;
    std::string game_host_;
    std::uint16_t game_port_;
    std::string game_server_;
};

} // namespace gs::server
