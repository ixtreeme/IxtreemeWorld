#pragma once

#include "CharacterListItem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace client::net {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct EntitySpawnInfo {
    std::uint32_t netId = 0;
    std::string name;
    std::uint16_t classId = 0;
    std::uint32_t mobTypeId = 0;
    std::uint32_t level = 1;
    float hpCurrent = 1.0f;
    float hpMax = 1.0f;
    Vec3 spawnPos;
    std::uint16_t heading = 0;
};

struct EntityHealthInfo {
    std::uint32_t netId = 0;
    float hpCurrent = 0.0f;
    float hpMax = 1.0f;
};

enum class MoveState : std::uint8_t {
    Idle = 0,
    Walking = 1,
    Running = 2,
};

struct EntityTransform {
    std::uint32_t netId = 0;
    Vec3 position;
    std::uint16_t heading = 0;
    MoveState moveState = MoveState::Idle;
};

struct IClientHandler {
    virtual ~IClientHandler() = default;

    virtual void OnConnectionFailed(const std::string& reason) = 0;
    virtual void OnDisconnected() = 0;

    virtual void OnHandshakeAccepted() = 0;
    virtual void OnHandshakeRejected(const std::string& reason) = 0;

    virtual void OnLoginAccepted(std::uint64_t account_id) = 0;
    virtual void OnLoginRejected(const std::string& reason) = 0;

    virtual void OnCharacterList(const std::vector<CharacterListItem>& characters) = 0;

    virtual void OnEnterWorldToken(std::vector<std::uint8_t> token,
                                   const std::string& host,
                                   std::uint16_t port) = 0;
    virtual void OnEnterWorldAccepted(std::uint32_t net_id, Vec3 spawn_pos) = 0;
    virtual void OnEnterWorldRejected(const std::string& reason) = 0;
    virtual void OnEntitySpawn(const EntitySpawnInfo& entity) = 0;
    virtual void OnEntityDespawn(std::uint32_t net_id) = 0;
    virtual void OnEntityHealthUpdate(const EntityHealthInfo& health) = 0;
    virtual void OnEntityDeath(std::uint32_t net_id, std::uint32_t killer_net_id) = 0;
    virtual void OnEntityTransforms(std::uint32_t server_tick,
                                    const std::vector<EntityTransform>& transforms) = 0;
};

} // namespace client::net
