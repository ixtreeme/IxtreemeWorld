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
    Vec3 spawnPos;
    std::uint16_t heading = 0;
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
};

} // namespace client::net
