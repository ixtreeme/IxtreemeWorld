#pragma once

#include "InputEvent.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class VulkanDevice;

namespace client::asset
{
class IAssetReader;
}

struct RmlHudData
{
    std::string playerName = "Player";
    int playerLevel = 1;
    float currentHp = 1.0f;
    float maxHp = 1.0f;
    float currentMp = 50.0f;
    float maxMp = 50.0f;
    int currentXp = 0;
    int xpForNextLevel = 1000;
    bool hasTarget = false;
    std::string targetName;
    int targetLevel = 1;
    float targetCurrentHp = 0.0f;
    float targetMaxHp = 1.0f;
    std::string zoneName = "Starting Zone";
    float playerX = 0.0f;
    float playerZ = 0.0f;
};

struct RmlCharacterListItem
{
    std::uint64_t id = 0;
    std::uint8_t slot = 0;
    std::string name;
    std::uint32_t level = 0;
    std::uint16_t classId = 0;
    std::uint16_t appearance = 0;
    std::int32_t posX = 0;
    std::int32_t posY = 0;
    std::uint16_t mapId = 0;
};

class RmlUiLayer
{
public:
    RmlUiLayer();
    ~RmlUiLayer();

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width, uint32_t height);
    void Update();
    void Render(VulkanDevice& device);
    void Resize(uint32_t width, uint32_t height);
    void OnRenderPassChanged(VulkanDevice& device);
    bool OnInput(const InputEvent& event);
    void HideAll();
    void SetLoginSubmitCallback(std::function<void(const std::string&, const std::string&, bool)> callback);
    void ShowLogin();
    void HideLogin();
    void SetLoginError(const std::string& message);
    bool IsLoginVisible() const;
    void SetLobbyCallbacks(std::function<void(std::uint64_t)> enterWorldCallback,
                           std::function<void()> newCharacterCallback,
                           std::function<void(std::uint64_t)> deleteCharacterCallback,
                           std::function<void()> logoutCallback);
    void ShowLobby();
    void HideLobby();
    void SetLobbyCharacters(const std::vector<RmlCharacterListItem>& characters);
    void SetLobbyStatus(const std::string& message);
    bool IsLobbyVisible() const;
    void ShowHud();
    void HideHud();
    void UpdateHud(const RmlHudData& data);
    bool IsHudVisible() const;
    void SetInGameMenuCallbacks(std::function<void()> resumeCallback,
                                std::function<void()> logoutCallback,
                                std::function<void()> quitCallback);
    void ShowInGameMenu();
    void HideInGameMenu();
    void ToggleInGameMenu();
    bool IsInGameMenuVisible() const;
    void ShowSettings();
    void HideSettings();
    bool IsSettingsVisible() const;
    void ShowInventory();
    void HideInventory();
    void ToggleInventory();
    bool IsInventoryVisible() const;
    void ShowCharacterCreation();
    void HideCharacterCreation();
    bool IsCharacterCreationVisible() const;
    void Destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
