#include "GameConnectionHandler.h"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include <capnp/message.h>
#include <kj/exception.h>
#include <sodium.h>

#include "common/Logging.h"
#include "db/Types.h"
#include "protocol/Protocol.h"
#include "protocol/Serialization.h"

namespace gs::game {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

#ifndef MMO_DEBUG_SPAWN_OVERRIDE
#define MMO_DEBUG_SPAWN_OVERRIDE 0
#endif

std::uint16_t ReadU16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

float DequantizeHeading(std::uint16_t value)
{
    return (static_cast<float>(value) / 65535.0f) * kTwoPi;
}

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

std::string Sha256Hex(const kj::ArrayPtr<const kj::byte>& bytes)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(bytes.begin()), bytes.size());
    return HexEncode(hash.data(), hash.size());
}

gs::protocol::S2cEnterWorldReject::RejectReason ToRejectReason(
    gs::db::HandoffTokenConsumeError error)
{
    switch (error) {
    case gs::db::HandoffTokenConsumeError::InvalidToken:
        return gs::protocol::S2cEnterWorldReject::RejectReason::INVALID_TOKEN;
    case gs::db::HandoffTokenConsumeError::ExpiredToken:
        return gs::protocol::S2cEnterWorldReject::RejectReason::EXPIRED_TOKEN;
    case gs::db::HandoffTokenConsumeError::AlreadyUsed:
        return gs::protocol::S2cEnterWorldReject::RejectReason::ALREADY_USED;
    default:
        return gs::protocol::S2cEnterWorldReject::RejectReason::SERVER_ERROR;
    }
}

} // namespace

GameConnectionHandler::GameConnectionHandler(gs::db::HandoffTokenRepository& handoff_tokens,
                                              gs::db::CharacterRepository& characters,
                                              WorldRuntime& sim,
                                              std::string game_server)
    : handoff_tokens_(handoff_tokens)
    , characters_(characters)
    , sim_(sim)
    , game_server_(std::move(game_server))
{
}

void GameConnectionHandler::OnPayload(std::shared_ptr<gs::network::Session> session,
                                      std::vector<std::uint8_t> payload)
{
    if (!payload.empty() && payload[0] == gs::protocol::kCodecBinary) {
        if (payload.size() != 9 || payload[1] != 0x01) {
            Disconnect(session, "invalid binary packet");
            return;
        }

        const auto sequence = ReadU32(payload.data() + 2);
        const auto heading_q = ReadU16(payload.data() + 6);
        const auto raw_state = payload[8];
        if (raw_state > static_cast<std::uint8_t>(MoveState::Running)) {
            Disconnect(session, "invalid move state");
            return;
        }

        sim_.PostMoveInput(session->Id(),
                           sequence,
                           DequantizeHeading(heading_q),
                           static_cast<MoveState>(raw_state));
        return;
    }

    std::lock_guard lock(contexts_mutex_);
    auto& ctx = contexts_[session->Id()];

    try {
        auto parsed = gs::protocol::ParsePacket(payload);
        if (!parsed) {
            Disconnect(session, "invalid message");
            return;
        }

        auto packet = parsed->packet;
        switch (ctx.state) {
        case GameSessionState::WaitingHandshake:
            if (!packet.isHandshakeRequest()) {
                Disconnect(session, "wrong state");
                return;
            }
            HandleHandshakeRequest(session, ctx, packet.getHandshakeRequest());
            return;
        case GameSessionState::ConnectionEstablished:
            if (!packet.isEnterWorld()) {
                Disconnect(session, "wrong state");
                return;
            }
            HandleEnterWorld(session, ctx, packet.getEnterWorld());
            return;
        case GameSessionState::EnteringWorld:
            Disconnect(session, "packet during enter world");
            return;
        case GameSessionState::InWorld:
            if (packet.isAttackTarget()) {
                sim_.PostAttackTarget(session->Id(), packet.getAttackTarget().getTargetNetId());
                return;
            }
            LOG_DEBUG("Session {} sent ignored in-world packet", session->Id());
            return;
        }
    } catch (const kj::Exception& error) {
        LOG_WARN("Session {} sent invalid Cap'n Proto message: {}",
                 session->Id(),
                 error.getDescription().cStr());
        Disconnect(session, "invalid message");
    }
}

void GameConnectionHandler::OnDisconnect(std::shared_ptr<gs::network::Session> session)
{
    {
        std::lock_guard lock(contexts_mutex_);
        contexts_.erase(session->Id());
    }
    LOG_INFO("Game session {} disconnected; posting sim despawn", session->Id());
    sim_.PostDespawn(session->Id());
    LOG_INFO("Game session {} cleaned up", session->Id());
}

void GameConnectionHandler::HandleHandshakeRequest(
    std::shared_ptr<gs::network::Session> session,
    GameSessionContext& ctx,
    gs::protocol::HandshakeRequest::Reader request)
{
    if (request.getProtocolVersion() != gs::protocol::kProtocolVersion) {
        SendHandshakeResponse(session,
                              gs::protocol::HandshakeResult::PROTOCOL_VERSION_MISMATCH,
                              "Protocol version mismatch",
                              true);
        return;
    }

    ctx.state = GameSessionState::ConnectionEstablished;
    SendHandshakeResponse(session, gs::protocol::HandshakeResult::OK, "Welcome to world");
    LOG_INFO("Game session {} handshake OK (build {})",
             session->Id(),
             request.getClientBuild().cStr());
}

void GameConnectionHandler::HandleEnterWorld(std::shared_ptr<gs::network::Session> session,
                                             GameSessionContext& ctx,
                                             gs::protocol::C2sEnterWorld::Reader request)
{
    ctx.state = GameSessionState::EnteringWorld;
    const auto token_hash = Sha256Hex(request.getToken());
    std::optional<DebugSpawnOverride> debug_spawn;
#if MMO_DEBUG_SPAWN_OVERRIDE
    if (request.hasDebugSpawnOverride()) {
        auto requested = request.getDebugSpawnOverride();
        debug_spawn = DebugSpawnOverride{requested.getX(), requested.getY()};
        LOG_INFO("Session {} requested debug spawn override: ({}, {})",
                 session->Id(),
                 debug_spawn->x,
                 debug_spawn->y);
    }
#endif

    handoff_tokens_.Consume(
        token_hash,
        game_server_,
        [this, session, debug_spawn](gs::db::HandoffTokenConsumeResult consume) {
            if (!consume) {
                LOG_WARN("Session {} enter world rejected: {}",
                         session->Id(),
                         gs::db::HandoffTokenConsumeErrorString(consume.error));
                SendEnterWorldReject(session, ToRejectReason(consume.error), true);
                return;
            }

            characters_.FindById(
                consume.data.character_id,
                [this, session, debug_spawn](gs::db::Result<gs::db::Character> character_result) {
                    std::lock_guard lock(contexts_mutex_);
                    const auto it = contexts_.find(session->Id());
                    if (it == contexts_.end()) {
                        return;
                    }

                    if (!character_result || !character_result.value) {
                        LOG_ERROR("Session {} enter world character load failed: {}",
                                  session->Id(),
                                  gs::db::DbErrorString(character_result.error));
                        SendEnterWorldReject(
                            session,
                            gs::protocol::S2cEnterWorldReject::RejectReason::SERVER_ERROR,
                            true);
                        return;
                    }

                    it->second.state = GameSessionState::InWorld;
                    sim_.PostSpawn(session, std::move(*character_result.value), debug_spawn);
                });
        });
}

void GameConnectionHandler::SendHandshakeResponse(std::shared_ptr<gs::network::Session> session,
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

void GameConnectionHandler::SendEnterWorldReject(
    std::shared_ptr<gs::network::Session> session,
    gs::protocol::S2cEnterWorldReject::RejectReason reason,
    bool close_after_send)
{
    capnp::MallocMessageBuilder msg;
    auto packet = msg.initRoot<gs::protocol::Packet>();
    auto reject = packet.initEnterWorldReject();
    reject.setReason(reason);
    if (close_after_send) {
        session->SendPayloadAndClose(gs::protocol::SerializeToBytes(msg));
    } else {
        session->SendPayload(gs::protocol::SerializeToBytes(msg));
    }
}

void GameConnectionHandler::Disconnect(std::shared_ptr<gs::network::Session> session,
                                       const std::string& reason)
{
    LOG_INFO("Disconnecting game session {}: {}", session->Id(), reason);
    session->Stop();
}

} // namespace gs::game
