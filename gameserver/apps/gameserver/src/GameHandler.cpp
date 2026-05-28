#include "GameHandler.h"

#include <cstdint>
#include <vector>

#include <capnp/message.h>

#include "common/Logging.h"
#include "protocol/Serialization.h"

namespace gs::server {

GameHandler::GameHandler(gs::db::CharacterRepository& characters)
    : characters_(characters)
{
}

void GameHandler::HandleCharacterListRequest(std::shared_ptr<gs::network::Session> session,
                                             const AuthInfo& auth)
{
    LOG_INFO("Session {} character list request (account_id={})",
             session->Id(),
             gs::db::ToUint64(auth.account_id));

    characters_.ListByAccount(
        auth.account_id,
        [this, session](gs::db::Result<std::vector<gs::db::Character>> result) {
            if (!result || !result.value) {
                LOG_WARN("Session {} character list failed: {}",
                         session->Id(),
                         gs::db::DbErrorString(result.error));
                SendCharacterListResponse(session,
                                          gs::protocol::CharacterListResult::INTERNAL_ERROR,
                                          "Failed to load characters",
                                          {});
                return;
            }

            LOG_INFO("Session {} sending {} characters", session->Id(), result.value->size());
            SendCharacterListResponse(session,
                                      gs::protocol::CharacterListResult::OK,
                                      "OK",
                                      *result.value);
        });
}

void GameHandler::SendCharacterListResponse(
    std::shared_ptr<gs::network::Session> session,
    gs::protocol::CharacterListResult result,
    const std::string& message,
    const std::vector<gs::db::Character>& characters)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto response = packet.initCharacterListResponse();
    response.setResult(result);
    response.setMessage(message);

    auto list = response.initCharacters(static_cast<unsigned int>(characters.size()));
    for (unsigned int i = 0; i < characters.size(); ++i) {
        const auto& character = characters[i];
        auto item = list[i];
        item.setId(gs::db::ToUint64(character.id));
        item.setSlot(character.slot);
        item.setName(character.name);
        item.setLevel(character.level);
        item.setClassId(character.class_id);
        item.setAppearance(character.appearance);
        item.setPosX(character.pos_x);
        item.setPosY(character.pos_y);
        item.setMapId(character.map_id);
    }

    session->SendPayload(gs::protocol::SerializeToBytes(msg));
}

} // namespace gs::server
