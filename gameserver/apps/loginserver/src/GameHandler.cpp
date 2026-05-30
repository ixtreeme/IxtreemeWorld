#include "GameHandler.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <sodium.h>

#include "common/Logging.h"
#include "protocol/Serialization.h"

namespace gs::server {
namespace {

std::string HexEncode(const unsigned char* bytes, std::size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    return out;
}

std::string Sha256Hex(const std::string& value)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(),
                       reinterpret_cast<const unsigned char*>(value.data()),
                       value.size());
    return HexEncode(hash.data(), hash.size());
}

std::string GenerateRawToken()
{
    std::array<unsigned char, 32> random{};
    randombytes_buf(random.data(), random.size());
    return HexEncode(random.data(), random.size());
}

} // namespace

GameHandler::GameHandler(gs::db::CharacterRepository& characters,
                         gs::db::HandoffTokenRepository& handoff_tokens,
                         std::string game_host,
                         std::uint16_t game_port,
                         std::string game_server)
    : characters_(characters)
    , handoff_tokens_(handoff_tokens)
    , game_host_(std::move(game_host))
    , game_port_(game_port)
    , game_server_(std::move(game_server))
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

void GameHandler::HandleCharacterSelect(std::shared_ptr<gs::network::Session> session,
                                        const AuthInfo& auth,
                                        gs::protocol::C2sCharacterSelect::Reader request)
{
    const auto character_id = gs::db::CharacterId{request.getCharacterId()};
    LOG_INFO("Session {} character select request: account_id={}, character_id={}",
             session->Id(),
             gs::db::ToUint64(auth.account_id),
             gs::db::ToUint64(character_id));

    characters_.FindByAccountAndId(
        auth.account_id,
        character_id,
        [this, session, auth, character_id](gs::db::Result<gs::db::Character> result) {
            if (!result || !result.value) {
                LOG_WARN("Session {} rejected character select: {}",
                         session->Id(),
                         gs::db::DbErrorString(result.error));
                return;
            }

            const auto raw_token = GenerateRawToken();
            const auto token_hash = Sha256Hex(raw_token);
            handoff_tokens_.Store(
                token_hash,
                auth.account_id,
                character_id,
                game_server_,
                30,
                [this, session, raw_token](gs::db::VoidResult store_result) {
                    if (!store_result) {
                        LOG_ERROR("Session {} failed to store handoff token: {}",
                                  session->Id(),
                                  store_result.message);
                        return;
                    }

                    LOG_INFO("Session {} received handoff token for {}:{}",
                             session->Id(),
                             game_host_,
                             game_port_);
                    SendEnterWorldToken(session, raw_token);
                });
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

void GameHandler::SendEnterWorldToken(std::shared_ptr<gs::network::Session> session,
                                      const std::string& token)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto response = packet.initEnterWorldToken();
    response.setToken(kj::ArrayPtr<const kj::byte>(
        reinterpret_cast<const kj::byte*>(token.data()), token.size()));
    response.setGameHost(game_host_);
    response.setGamePort(game_port_);
    session->SendPayload(gs::protocol::SerializeToBytes(msg));
}

} // namespace gs::server
