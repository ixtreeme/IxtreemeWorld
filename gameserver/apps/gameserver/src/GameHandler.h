#pragma once

#include <memory>
#include <string>
#include <vector>

#include <db/CharacterRepository.h>
#include <network/Session.h>

#include "SessionContext.h"
#include "schema/packet.capnp.h"

namespace gs::server {

class GameHandler {
public:
    explicit GameHandler(gs::db::CharacterRepository& characters);

    void HandleCharacterListRequest(std::shared_ptr<gs::network::Session> session,
                                    const AuthInfo& auth);

private:
    void SendCharacterListResponse(std::shared_ptr<gs::network::Session> session,
                                   gs::protocol::CharacterListResult result,
                                   const std::string& message,
                                   const std::vector<gs::db::Character>& characters);

    gs::db::CharacterRepository& characters_;
};

} // namespace gs::server
