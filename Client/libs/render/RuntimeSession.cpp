#include "RuntimeSession.h"

namespace
{
class EmptyRuntimeSession final : public RuntimeSession
{
public:
    bool Create(VulkanDevice&, client::asset::IAssetReader&, uint32_t, uint32_t) override { return true; }
    void Destroy() override {}
    void SetQuitCallback(std::function<void()> callback) override { m_quitCallback = std::move(callback); }
    void Update(double) override {}
    void UpdateNetwork() override {}
    void SendMoveInput(float, RuntimeMoveState) override {}
    void SendAttackTarget(std::uint32_t) override {}
    void OnRenderPassChanged(VulkanDevice&) override {}
    void Resize(uint32_t, uint32_t) override {}

    void Start(const SceneData&) override { m_playing = true; }
    void Tick(double) override {}
    void Stop() override
    {
        m_playing = false;
        m_entities.clear();
    }
    void OnSceneLoaded(const SceneData&) override {}

    bool IsLobbyActive() const override { return false; }
    bool IsInWorld() const override { return m_playing || m_mapEditorOpen; }
    std::vector<WorldRenderEntity> GetWorldEntities() const override { return m_entities; }

    bool IsMapEditorOpen() const override { return m_mapEditorOpen; }
    void SetMapEditorOpen(bool open) override { m_mapEditorOpen = open; }
    void ToggleMapEditor() override { m_mapEditorOpen = !m_mapEditorOpen; }
    bool IsTextInputFocused() const override { return false; }
    void ClearKeyboardFocus() override {}
    bool OnInput(const InputEvent&) override { return false; }
    MapEditorSettings GetMapEditorSettings() const override { return {}; }
    MapEditorCommands ConsumeMapEditorCommands() override { return {}; }
    LightingState GetLightingState() const override { return {}; }
    void SetDynamicLightEditorState(const DynamicLightEditorState&) override {}
    void SetWaterBodyEditorState(const WaterBodyEditorState&) override {}
    void InitializeAssetLibrary(const std::string&, const std::array<MapEditorPaletteSlot, 8>& defaultSlots) override
    {
        m_paletteSlots = defaultSlots;
    }
    std::vector<std::pair<std::string, WaterMaterialData>> GetWaterMaterialsSnapshot() const override { return {}; }
    std::array<MapEditorPaletteSlot, 8> GetPaletteSlots() const override { return m_paletteSlots; }
    void SetEditorStatus(const std::string& status) override { m_editorStatus = status; }
    void ImportDroppedFiles(const std::vector<std::string>&) override {}

private:
    std::function<void()> m_quitCallback;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::vector<WorldRenderEntity> m_entities;
    std::string m_editorStatus;
    bool m_playing = false;
    bool m_mapEditorOpen = false;
};
}

std::unique_ptr<RuntimeSession> CreateEmptyRuntimeSession()
{
    return std::make_unique<EmptyRuntimeSession>();
}
