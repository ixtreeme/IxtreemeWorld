#pragma once

#include "GameClientLayer.h"
#include "SceneManager.h"
#include "network/ClientSession.h"

#include <memory>
#include <optional>

class RuntimeSession
{
public:
    virtual ~RuntimeSession() = default;

    virtual bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width, uint32_t height) = 0;
    virtual void Destroy() = 0;
    virtual void SetDebugSpawnOverride(std::optional<client::net::DebugSpawnOverride> override) = 0;
    virtual void SetQuitCallback(std::function<void()> callback) = 0;
    virtual void Update(double timeSeconds) = 0;
    virtual void UpdateNetwork() = 0;
    virtual void SendMoveInput(float directionAngle, client::net::MoveState moveState) = 0;
    virtual void SendAttackTarget(std::uint32_t netId) = 0;
    virtual void OnRenderPassChanged(VulkanDevice& device) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    virtual void Start(const SceneData& openScene) = 0;
    virtual void Tick(double deltaSeconds) = 0;
    virtual void Stop() = 0;
    virtual void OnSceneLoaded(const SceneData& scene) = 0;

    virtual bool IsLobbyActive() const = 0;
    virtual bool IsInWorld() const = 0;
    virtual bool IsLocalPlayMode() const = 0;
    virtual std::uint32_t GetOwnNetId() const = 0;
    virtual std::vector<WorldRenderEntity> GetWorldEntities() const = 0;
    virtual void EnterLocalPlayMode(const WorldRenderEntity& player) = 0;
    virtual void UpdateLocalPlayPlayer(client::net::Vec3 position,
                                       std::uint16_t heading,
                                       client::net::MoveState moveState) = 0;
    virtual void ExitLocalPlayMode() = 0;

    virtual bool IsMapEditorOpen() const = 0;
    virtual void SetMapEditorOpen(bool open) = 0;
    virtual void ToggleMapEditor() = 0;
    virtual bool IsTextInputFocused() const = 0;
    virtual void ClearKeyboardFocus() = 0;
    virtual bool OnInput(const InputEvent& event) = 0;
    virtual MapEditorSettings GetMapEditorSettings() const = 0;
    virtual MapEditorCommands ConsumeMapEditorCommands() = 0;
    virtual LightingState GetLightingState() const = 0;
    virtual void SetDynamicLightEditorState(const DynamicLightEditorState& state) = 0;
    virtual void SetWaterBodyEditorState(const WaterBodyEditorState& state) = 0;
    virtual void InitializeAssetLibrary(const std::string& mapDirectory,
                                        const std::array<MapEditorPaletteSlot, 8>& defaultSlots) = 0;
    virtual std::vector<std::pair<std::string, WaterMaterialData>> GetWaterMaterialsSnapshot() const = 0;
    virtual std::array<MapEditorPaletteSlot, 8> GetPaletteSlots() const = 0;
    virtual void SetEditorStatus(const std::string& status) = 0;
    virtual void ImportDroppedFiles(const std::vector<std::string>& paths) = 0;

    virtual void SetLoginCallbacks(std::function<void()> acceptedCallback,
                                   std::function<void(const std::string&)> statusCallback) = 0;
    virtual void SubmitLogin(const std::string& username, const std::string& password, bool remember) = 0;
    virtual void SetLobbyCallbacks(std::function<void()> shownCallback,
                                   std::function<void(const std::vector<client::net::CharacterListItem>&)> charactersCallback,
                                   std::function<void(const std::string&)> statusCallback,
                                   std::function<void()> enteredWorldCallback) = 0;
    virtual void EnterWorldWithCharacter(std::uint64_t characterId) = 0;
    virtual void LogoutToLogin() = 0;
};

std::unique_ptr<RuntimeSession> CreateEmptyRuntimeSession();
std::unique_ptr<RuntimeSession> CreateAurigaRuntimeSession();
