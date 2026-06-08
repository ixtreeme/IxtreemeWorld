#include "RuntimeSession.h"

#include "network/ClientSession.h"

namespace
{
class EmptyRuntimeSession final : public RuntimeSession
{
public:
    bool Create(VulkanDevice&, client::asset::IAssetReader&, uint32_t, uint32_t) override { return true; }
    void Destroy() override {}
    void SetDebugSpawnOverride(std::optional<client::net::DebugSpawnOverride>) override {}
    void SetQuitCallback(std::function<void()> callback) override { m_quitCallback = std::move(callback); }
    void Update(double) override {}
    void UpdateNetwork() override {}
    void SendMoveInput(float, client::net::MoveState) override {}
    void SendAttackTarget(std::uint32_t) override {}
    void OnRenderPassChanged(VulkanDevice&) override {}
    void Resize(uint32_t, uint32_t) override {}

    void Start(const SceneData&) override { m_playing = true; }
    void Tick(double) override {}
    void Stop() override
    {
        m_playing = false;
        m_localPlayMode = false;
        m_entities.clear();
        m_ownNetId = 0;
    }
    void OnSceneLoaded(const SceneData&) override {}

    bool IsLobbyActive() const override { return false; }
    bool IsInWorld() const override { return m_playing && m_localPlayMode; }
    bool IsLocalPlayMode() const override { return m_localPlayMode; }
    std::uint32_t GetOwnNetId() const override { return m_ownNetId; }
    std::vector<WorldRenderEntity> GetWorldEntities() const override { return m_entities; }
    void EnterLocalPlayMode(const WorldRenderEntity& player) override
    {
        m_localPlayMode = true;
        m_ownNetId = player.netId;
        m_entities = {player};
    }
    void UpdateLocalPlayPlayer(client::net::Vec3 position,
                               std::uint16_t heading,
                               client::net::MoveState moveState) override
    {
        if (m_entities.empty())
            return;
        m_entities.front().position = position;
        m_entities.front().heading = heading;
        m_entities.front().moveState = moveState;
    }
    void ExitLocalPlayMode() override
    {
        m_localPlayMode = false;
        m_entities.clear();
        m_ownNetId = 0;
    }

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

    void SetLoginCallbacks(std::function<void()>, std::function<void(const std::string&)>) override {}
    void SubmitLogin(const std::string&, const std::string&, bool) override {}
    void SetLobbyCallbacks(std::function<void()>,
                           std::function<void(const std::vector<client::net::CharacterListItem>&)>,
                           std::function<void(const std::string&)>,
                           std::function<void()>) override {}
    void EnterWorldWithCharacter(std::uint64_t) override {}
    void LogoutToLogin() override { Stop(); }

private:
    std::function<void()> m_quitCallback;
    std::array<MapEditorPaletteSlot, 8> m_paletteSlots{};
    std::vector<WorldRenderEntity> m_entities;
    std::string m_editorStatus;
    std::uint32_t m_ownNetId = 0;
    bool m_playing = false;
    bool m_localPlayMode = false;
    bool m_mapEditorOpen = false;
};

class AurigaRuntimeSession final : public RuntimeSession
{
public:
    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width, uint32_t height) override
    {
        if (!m_gameClient.Create(device, assets, width, height))
            return false;
        m_clientSession = std::make_unique<client::net::ClientSession>(m_gameClient);
        if (m_debugSpawnOverride)
            m_clientSession->SetDebugSpawnOverride(m_debugSpawnOverride);
        m_gameClient.SetClientSession(m_clientSession.get());
        return true;
    }

    void Destroy() override
    {
        if (m_clientSession)
            m_clientSession->Disconnect();
        m_gameClient.Destroy();
        m_clientSession.reset();
    }

    void SetDebugSpawnOverride(std::optional<client::net::DebugSpawnOverride> override) override
    {
        m_debugSpawnOverride = std::move(override);
        if (m_clientSession)
            m_clientSession->SetDebugSpawnOverride(m_debugSpawnOverride);
    }

    void SetQuitCallback(std::function<void()> callback) override { m_gameClient.SetQuitCallback(std::move(callback)); }
    void Update(double timeSeconds) override { m_gameClient.Update(timeSeconds); }
    void UpdateNetwork() override { if (m_clientSession) m_clientSession->Update(); }
    void SendMoveInput(float directionAngle, client::net::MoveState moveState) override
    {
        if (m_clientSession)
            m_clientSession->SendMoveInput(directionAngle, moveState);
    }
    void SendAttackTarget(std::uint32_t netId) override
    {
        if (m_clientSession)
            m_clientSession->SendAttackTarget(netId);
    }
    void OnRenderPassChanged(VulkanDevice& device) override { m_gameClient.OnRenderPassChanged(device); }
    void Resize(uint32_t width, uint32_t height) override { m_gameClient.Resize(width, height); }

    void Start(const SceneData&) override {}
    void Tick(double) override {}
    void Stop() override
    {
        if (m_gameClient.IsLocalPlayMode())
            m_gameClient.ExitLocalPlayMode();
        else if (m_gameClient.IsInWorld() || m_gameClient.IsLobbyActive())
            m_gameClient.LogoutToLogin();
    }
    void OnSceneLoaded(const SceneData&) override {}

    bool IsLobbyActive() const override { return m_gameClient.IsLobbyActive(); }
    bool IsInWorld() const override { return m_gameClient.IsInWorld(); }
    bool IsLocalPlayMode() const override { return m_gameClient.IsLocalPlayMode(); }
    std::uint32_t GetOwnNetId() const override { return m_gameClient.GetOwnNetId(); }
    std::vector<WorldRenderEntity> GetWorldEntities() const override { return m_gameClient.GetWorldEntities(); }
    void EnterLocalPlayMode(const WorldRenderEntity& player) override { m_gameClient.EnterLocalPlayMode(player); }
    void UpdateLocalPlayPlayer(client::net::Vec3 position,
                               std::uint16_t heading,
                               client::net::MoveState moveState) override
    {
        m_gameClient.UpdateLocalPlayPlayer(position, heading, moveState);
    }
    void ExitLocalPlayMode() override { m_gameClient.ExitLocalPlayMode(); }

    bool IsMapEditorOpen() const override { return m_gameClient.IsMapEditorOpen(); }
    void SetMapEditorOpen(bool open) override { m_gameClient.SetMapEditorOpen(open); }
    void ToggleMapEditor() override { m_gameClient.ToggleMapEditor(); }
    bool IsTextInputFocused() const override { return m_gameClient.IsTextInputFocused(); }
    void ClearKeyboardFocus() override { m_gameClient.ClearKeyboardFocus(); }
    bool OnInput(const InputEvent& event) override { return m_gameClient.OnInput(event); }
    MapEditorSettings GetMapEditorSettings() const override { return m_gameClient.GetMapEditorSettings(); }
    MapEditorCommands ConsumeMapEditorCommands() override { return m_gameClient.ConsumeMapEditorCommands(); }
    LightingState GetLightingState() const override { return m_gameClient.GetLightingState(); }
    void SetDynamicLightEditorState(const DynamicLightEditorState& state) override { m_gameClient.SetDynamicLightEditorState(state); }
    void SetWaterBodyEditorState(const WaterBodyEditorState& state) override { m_gameClient.SetWaterBodyEditorState(state); }
    void InitializeAssetLibrary(const std::string& mapDirectory,
                                const std::array<MapEditorPaletteSlot, 8>& defaultSlots) override
    {
        m_gameClient.InitializeAssetLibrary(mapDirectory, defaultSlots);
    }
    std::vector<std::pair<std::string, WaterMaterialData>> GetWaterMaterialsSnapshot() const override
    {
        return m_gameClient.GetWaterMaterialsSnapshot();
    }
    std::array<MapEditorPaletteSlot, 8> GetPaletteSlots() const override { return m_gameClient.GetPaletteSlots(); }
    void SetEditorStatus(const std::string& status) override { m_gameClient.SetEditorStatus(status); }
    void ImportDroppedFiles(const std::vector<std::string>& paths) override { m_gameClient.ImportDroppedFiles(paths); }

    void SetLoginCallbacks(std::function<void()> acceptedCallback,
                           std::function<void(const std::string&)> statusCallback) override
    {
        m_gameClient.SetLoginCallbacks(std::move(acceptedCallback), std::move(statusCallback));
    }
    void SubmitLogin(const std::string& username, const std::string& password, bool remember) override
    {
        m_gameClient.SubmitLogin(username, password, remember);
    }
    void SetLobbyCallbacks(std::function<void()> shownCallback,
                           std::function<void(const std::vector<client::net::CharacterListItem>&)> charactersCallback,
                           std::function<void(const std::string&)> statusCallback,
                           std::function<void()> enteredWorldCallback) override
    {
        m_gameClient.SetLobbyCallbacks(std::move(shownCallback),
            std::move(charactersCallback),
            std::move(statusCallback),
            std::move(enteredWorldCallback));
    }
    void EnterWorldWithCharacter(std::uint64_t characterId) override { m_gameClient.EnterWorldWithCharacter(characterId); }
    void LogoutToLogin() override { m_gameClient.LogoutToLogin(); }

private:
    GameClientLayer m_gameClient;
    std::unique_ptr<client::net::ClientSession> m_clientSession;
    std::optional<client::net::DebugSpawnOverride> m_debugSpawnOverride;
};
}

std::unique_ptr<RuntimeSession> CreateEmptyRuntimeSession()
{
    return std::make_unique<EmptyRuntimeSession>();
}

std::unique_ptr<RuntimeSession> CreateAurigaRuntimeSession()
{
    return std::make_unique<AurigaRuntimeSession>();
}

