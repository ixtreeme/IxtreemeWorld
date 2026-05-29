#pragma once

#include "InputEvent.h"
#include "network/IClientHandler.h"
#include "network/LegacyBridge.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace client::net {
class ClientSession;
}

class VulkanDevice;

class NoesisLayer : public client::net::IClientHandler
{
public:
    NoesisLayer();
    ~NoesisLayer();

    bool Create(VulkanDevice& device, uint32_t width, uint32_t height);
    void Update(double timeSeconds);
    void RenderOffscreen(VulkanDevice& device);
    void RenderOnscreen(VulkanDevice& device);
    void OnRenderPassChanged(VulkanDevice& device);
    void Resize(uint32_t width, uint32_t height);
    bool IsLobbyActive() const;
    bool IsWorldActive() const;
    bool IsInGameMenuOpen() const;
    bool IsQuestPanelOpen() const;
    bool IsCharacterPanelOpen() const;
    void ToggleInGameMenu();
    void ToggleQuestPanel();
    void ToggleCharacterPanel();
    void SetQuitCallback(std::function<void()> callback);
    void SetClientSession(client::net::ClientSession* session);
    const TWorldEnterInfo& GetWorldEnterInfo() const;
    const std::vector<TWorldEntityInfo>& GetWorldEntities() const;
    bool OnInput(const InputEvent& event);

    void OnConnectionFailed(const std::string& reason) override;
    void OnDisconnected() override;
    void OnHandshakeAccepted() override;
    void OnHandshakeRejected(const std::string& reason) override;
    void OnLoginAccepted(uint64_t account_id) override;
    void OnLoginRejected(const std::string& reason) override;
    void OnCharacterList(const std::vector<client::net::CharacterListItem>& characters) override;

    void Destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
