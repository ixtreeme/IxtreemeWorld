#pragma once

#include "InputEvent.h"
#include "network/IClientHandler.h"
#include "WorldCamera.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace client::net {
class ClientSession;
}

namespace client::asset {
class IAssetReader;
}

class VulkanDevice;

struct WorldRenderEntity
{
    std::uint32_t netId = 0;
    std::string name;
    client::net::Vec3 position;
    std::uint16_t heading = 0;
    client::net::MoveState moveState = client::net::MoveState::Idle;
};

class NoesisLayer : public client::net::IClientHandler
{
public:
    NoesisLayer();
    ~NoesisLayer();

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width,
        uint32_t height);
    void Update(double timeSeconds);
    void RenderOffscreen(VulkanDevice& device);
    void RenderOnscreen(VulkanDevice& device);
    void OnRenderPassChanged(VulkanDevice& device);
    void Resize(uint32_t width, uint32_t height);
    bool IsLobbyActive() const;
    bool IsInWorld() const;
    std::uint32_t GetOwnNetId() const;
    std::vector<WorldRenderEntity> GetWorldEntities() const;
    bool IsInGameMenuOpen() const;
    void ToggleInGameMenu();
    void SetQuitCallback(std::function<void()> callback);
    void SetClientSession(client::net::ClientSession* session);
    bool OnInput(const InputEvent& event);

    void OnConnectionFailed(const std::string& reason) override;
    void OnDisconnected() override;
    void OnHandshakeAccepted() override;
    void OnHandshakeRejected(const std::string& reason) override;
    void OnLoginAccepted(uint64_t account_id) override;
    void OnLoginRejected(const std::string& reason) override;
    void OnCharacterList(const std::vector<client::net::CharacterListItem>& characters) override;
    void OnEnterWorldToken(std::vector<std::uint8_t> token,
                           const std::string& host,
                           std::uint16_t port) override;
    void OnEnterWorldAccepted(std::uint32_t net_id, client::net::Vec3 spawn_pos) override;
    void OnEnterWorldRejected(const std::string& reason) override;
    void OnEntitySpawn(const client::net::EntitySpawnInfo& entity) override;
    void OnEntityDespawn(std::uint32_t net_id) override;
    void OnEntityTransforms(std::uint32_t server_tick,
                            const std::vector<client::net::EntityTransform>& transforms) override;

    void Destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
