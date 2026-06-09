#pragma once

#include "InputEvent.h"
#include "MapEditorTypes.h"
#include "SceneManager.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class VulkanDevice;

namespace client::asset
{
class IAssetReader;
}

struct RuntimeVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class RuntimeMoveState : std::uint8_t
{
    Idle,
    Walking,
    Running,
};

struct WorldRenderEntity
{
    std::uint32_t netId = 0;
    std::string name;
    RuntimeVec3 position;
    std::uint16_t heading = 0;
    RuntimeMoveState moveState = RuntimeMoveState::Idle;
    std::uint32_t mobTypeId = 0;
    std::uint32_t level = 1;
    float hpCurrent = 1.0f;
    float hpMax = 1.0f;
    float hpDisplayed = 1.0f;
};

class RuntimeSession
{
public:
    virtual ~RuntimeSession() = default;

    virtual bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width, uint32_t height) = 0;
    virtual void Destroy() = 0;
    virtual void SetQuitCallback(std::function<void()> callback) = 0;
    virtual void Update(double timeSeconds) = 0;
    virtual void UpdateNetwork() = 0;
    virtual void SendMoveInput(float directionAngle, RuntimeMoveState moveState) = 0;
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
    virtual void UpdateLocalPlayPlayer(RuntimeVec3 position,
                                       std::uint16_t heading,
                                       RuntimeMoveState moveState) = 0;
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

};

std::unique_ptr<RuntimeSession> CreateEmptyRuntimeSession();
