#include "GameClientLayer.h"

#include "Debug.h"
#include "network/ClientSession.h"

#include <algorithm>
#include <cstdio>
#include <utility>

struct GameClientLayer::Impl
{
    client::net::ClientSession* clientSession = nullptr;
    std::function<void()> quitCallback;
    std::function<void()> loginAcceptedCallback;
    std::function<void(const std::string&)> loginStatusCallback;
    std::function<void()> lobbyShownCallback;
    std::function<void(const std::vector<client::net::CharacterListItem>&)> lobbyCharactersCallback;
    std::function<void(const std::string&)> lobbyStatusCallback;
    std::function<void()> lobbyEnteredWorldCallback;

    std::string loginName;
    std::string loginPassword;
    std::vector<std::uint8_t> pendingEnterWorldToken;
    std::string pendingGameHost;
    std::uint16_t pendingGamePort = 0;
    bool pendingGameConnect = false;
    bool pendingEnterWorldConnect = false;

    std::uint32_t currentWidth = 0;
    std::uint32_t currentHeight = 0;
    bool lobbyActive = false;
    bool inWorld = false;
    bool localPlayMode = false;
    bool localSavedStateValid = false;
    bool savedLobbyActive = false;
    bool savedInWorld = false;
    std::uint32_t savedOwnNetId = 0;
    std::vector<WorldRenderEntity> savedEntities;
    bool inGameMenuOpen = false;
    bool mapEditorOpen = false;
    std::uint32_t ownNetId = 0;
    double currentTimeSeconds = 0.0;

    std::vector<WorldRenderEntity> entities;
    std::array<MapEditorPaletteSlot, 8> paletteSlots{};
    LightingState lightingState;
    DynamicLightEditorState dynamicLightEditorState;
    WaterBodyEditorState waterBodyEditorState;

    WorldRenderEntity* FindEntity(std::uint32_t netId)
    {
        auto it = std::find_if(entities.begin(), entities.end(), [netId](const WorldRenderEntity& entity) {
            return entity.netId == netId;
        });
        return it == entities.end() ? nullptr : &*it;
    }

    WorldRenderEntity& UpsertEntity(std::uint32_t netId)
    {
        if (WorldRenderEntity* existing = FindEntity(netId))
            return *existing;

        WorldRenderEntity entity{};
        entity.netId = netId;
        entities.push_back(std::move(entity));
        return entities.back();
    }

    void SetLoginStatus(const std::string& text)
    {
        if (loginStatusCallback)
            loginStatusCallback(text);
    }

    void SetLobbyStatus(const std::string& text)
    {
        if (lobbyStatusCallback)
            lobbyStatusCallback(text);
    }

    void SubmitLogin(const std::string& username, const std::string& password, bool remember)
    {
        Tracenf("[LOGIN] attempt user='%s' remember=%s", username.c_str(), remember ? "true" : "false");
        if (username.empty() || password.empty())
        {
            SetLoginStatus("Username and password required");
            return;
        }
        if (!clientSession)
        {
            SetLoginStatus("Network session is not ready");
            return;
        }

        loginName = username;
        loginPassword = password;
        clientSession->Connect("159.195.56.82", 11000);
        SetLoginStatus("Connecting...");
    }
};

GameClientLayer::GameClientLayer() = default;
GameClientLayer::~GameClientLayer() { Destroy(); }

bool GameClientLayer::Create(VulkanDevice&, client::asset::IAssetReader&, uint32_t width, uint32_t height)
{
    m_impl = std::make_unique<Impl>();
    m_impl->currentWidth = width;
    m_impl->currentHeight = height;
    Tracen("[BUILD] Game client state layer initialized");
    return true;
}

void GameClientLayer::Update(double timeSeconds)
{
    if (!m_impl)
        return;

    m_impl->currentTimeSeconds = timeSeconds;
    if (m_impl->pendingGameConnect && m_impl->clientSession && !m_impl->clientSession->IsConnected())
    {
        m_impl->pendingGameConnect = false;
        m_impl->pendingEnterWorldConnect = true;
        Tracenf("[WORLD] deferred game connect to %s", m_impl->pendingGameHost.c_str());
        m_impl->clientSession->Connect(m_impl->pendingGameHost, m_impl->pendingGamePort);
    }
}

void GameClientLayer::OnRenderPassChanged(VulkanDevice&) {}

void GameClientLayer::Resize(uint32_t width, uint32_t height)
{
    if (!m_impl)
        return;
    m_impl->currentWidth = width;
    m_impl->currentHeight = height;
}

bool GameClientLayer::IsLobbyActive() const { return m_impl && m_impl->lobbyActive; }
bool GameClientLayer::IsInWorld() const { return m_impl && m_impl->inWorld; }
bool GameClientLayer::IsLocalPlayMode() const { return m_impl && m_impl->localPlayMode; }
std::uint32_t GameClientLayer::GetOwnNetId() const { return m_impl ? m_impl->ownNetId : 0; }
std::vector<WorldRenderEntity> GameClientLayer::GetWorldEntities() const { return m_impl ? m_impl->entities : std::vector<WorldRenderEntity>{}; }
void GameClientLayer::EnterLocalPlayMode(const WorldRenderEntity& player)
{
    if (!m_impl)
        return;

    m_impl->localPlayMode = true;
    m_impl->localSavedStateValid = true;
    m_impl->savedLobbyActive = m_impl->lobbyActive;
    m_impl->savedInWorld = m_impl->inWorld;
    m_impl->savedOwnNetId = m_impl->ownNetId;
    m_impl->savedEntities = m_impl->entities;
    m_impl->inWorld = true;
    m_impl->lobbyActive = false;
    m_impl->inGameMenuOpen = false;
    m_impl->ownNetId = player.netId;
    m_impl->entities.clear();
    m_impl->entities.push_back(player);
    Tracenf("[EDIT-PLAY] Test character spawned: id=%u pos=(%.1f,%.1f,%.1f)",
        player.netId,
        player.position.x,
        player.position.y,
        player.position.z);
}
void GameClientLayer::UpdateLocalPlayPlayer(client::net::Vec3 position,
                                            std::uint16_t heading,
                                            client::net::MoveState moveState)
{
    if (!m_impl || !m_impl->localPlayMode)
        return;

    WorldRenderEntity& player = m_impl->UpsertEntity(m_impl->ownNetId);
    player.position = position;
    player.heading = heading;
    player.moveState = moveState;
}
void GameClientLayer::ExitLocalPlayMode()
{
    if (!m_impl || !m_impl->localPlayMode)
        return;

    m_impl->localPlayMode = false;
    if (m_impl->localSavedStateValid)
    {
        m_impl->lobbyActive = m_impl->savedLobbyActive;
        m_impl->inWorld = m_impl->savedInWorld;
        m_impl->ownNetId = m_impl->savedOwnNetId;
        m_impl->entities = std::move(m_impl->savedEntities);
        m_impl->localSavedStateValid = false;
    }
    else
    {
        m_impl->inWorld = false;
        m_impl->ownNetId = 0;
        m_impl->entities.clear();
    }
    m_impl->inGameMenuOpen = false;
    Tracen("[EDIT-PLAY] Runtime state cleared");
}
bool GameClientLayer::IsInGameMenuOpen() const { return m_impl && m_impl->inGameMenuOpen; }
bool GameClientLayer::IsMapEditorOpen() const { return m_impl && m_impl->mapEditorOpen; }
void GameClientLayer::ToggleInGameMenu() { if (m_impl) m_impl->inGameMenuOpen = !m_impl->inGameMenuOpen; }
bool GameClientLayer::LoadMapEditorView(uint32_t, uint32_t) { return true; }
void GameClientLayer::SetMapEditorOpen(bool open) { if (m_impl) m_impl->mapEditorOpen = open; }
void GameClientLayer::ToggleMapEditor() { if (m_impl) m_impl->mapEditorOpen = !m_impl->mapEditorOpen; }
void GameClientLayer::ClearKeyboardFocus() {}
bool GameClientLayer::IsTextInputFocused() const { return false; }
MapEditorSettings GameClientLayer::GetMapEditorSettings() const { return {}; }
MapEditorCommands GameClientLayer::ConsumeMapEditorCommands() { return {}; }
LightingState GameClientLayer::GetLightingState() const { return m_impl ? m_impl->lightingState : LightingState{}; }
void GameClientLayer::SetDynamicLightEditorState(const DynamicLightEditorState& state) { if (m_impl) m_impl->dynamicLightEditorState = state; }
void GameClientLayer::SetWaterBodyEditorState(const WaterBodyEditorState& state) { if (m_impl) m_impl->waterBodyEditorState = state; }
void GameClientLayer::InitializeAssetLibrary(const std::string&, const std::array<MapEditorPaletteSlot, 8>& defaultSlots) { if (m_impl) m_impl->paletteSlots = defaultSlots; }
std::vector<std::pair<std::string, WaterMaterialData>> GameClientLayer::GetWaterMaterialsSnapshot() const { return {}; }
void GameClientLayer::ImportAsset(AssetLibrary::Category) {}
std::array<MapEditorPaletteSlot, 8> GameClientLayer::GetPaletteSlots() const { return m_impl ? m_impl->paletteSlots : std::array<MapEditorPaletteSlot, 8>{}; }
void GameClientLayer::SetEditorStatus(const std::string& status) { Tracenf("[EDITOR] %s", status.c_str()); }
void GameClientLayer::ImportDroppedFiles(const std::vector<std::string>&) {}
void GameClientLayer::SetQuitCallback(std::function<void()> callback) { if (m_impl) m_impl->quitCallback = std::move(callback); }
void GameClientLayer::SetClientSession(client::net::ClientSession* session) { if (m_impl) m_impl->clientSession = session; }
void GameClientLayer::SetLoginCallbacks(std::function<void()> acceptedCallback, std::function<void(const std::string&)> statusCallback)
{
    if (!m_impl) return;
    m_impl->loginAcceptedCallback = std::move(acceptedCallback);
    m_impl->loginStatusCallback = std::move(statusCallback);
}
void GameClientLayer::SubmitLogin(const std::string& username, const std::string& password, bool remember) { if (m_impl) m_impl->SubmitLogin(username, password, remember); }
void GameClientLayer::SetLobbyCallbacks(std::function<void()> shownCallback,
                                        std::function<void(const std::vector<client::net::CharacterListItem>&)> charactersCallback,
                                        std::function<void(const std::string&)> statusCallback,
                                        std::function<void()> enteredWorldCallback)
{
    if (!m_impl) return;
    m_impl->lobbyShownCallback = std::move(shownCallback);
    m_impl->lobbyCharactersCallback = std::move(charactersCallback);
    m_impl->lobbyStatusCallback = std::move(statusCallback);
    m_impl->lobbyEnteredWorldCallback = std::move(enteredWorldCallback);
}
void GameClientLayer::EnterWorldWithCharacter(std::uint64_t characterId)
{
    if (!m_impl || !m_impl->clientSession)
        return;
    m_impl->SetLobbyStatus("Entering world...");
    m_impl->clientSession->SendCharacterSelect(characterId);
}
void GameClientLayer::LogoutToLogin()
{
    if (!m_impl)
        return;
    m_impl->inWorld = false;
    m_impl->lobbyActive = false;
    m_impl->inGameMenuOpen = false;
    if (m_impl->clientSession)
        m_impl->clientSession->Disconnect();
    m_impl->SetLoginStatus("Disconnected");
}
bool GameClientLayer::OnInput(const InputEvent&) { return false; }

void GameClientLayer::OnConnectionFailed(const std::string& reason)
{
    if (!m_impl) return;
    if (m_impl->pendingEnterWorldConnect || m_impl->pendingGameConnect || m_impl->inWorld)
        m_impl->SetLobbyStatus(reason.empty() ? "Connection failed" : reason);
    else
        m_impl->SetLoginStatus(reason.empty() ? "Connection failed" : reason);
    m_impl->pendingGameConnect = false;
    m_impl->pendingEnterWorldConnect = false;
}
void GameClientLayer::OnDisconnected()
{
    if (m_impl && !m_impl->pendingGameConnect)
        m_impl->SetLoginStatus("Disconnected");
}
void GameClientLayer::OnHandshakeAccepted()
{
    if (!m_impl || !m_impl->clientSession)
        return;
    if (m_impl->pendingEnterWorldConnect)
    {
        m_impl->pendingEnterWorldConnect = false;
        m_impl->SetLobbyStatus("Entering world...");
        m_impl->clientSession->SendEnterWorld(m_impl->pendingEnterWorldToken);
        return;
    }
    m_impl->SetLoginStatus("Logging in...");
    m_impl->clientSession->SendLogin(m_impl->loginName, m_impl->loginPassword);
}
void GameClientLayer::OnHandshakeRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetLoginStatus(reason.empty() ? "Handshake rejected" : reason);
}
void GameClientLayer::OnLoginAccepted(uint64_t account_id)
{
    if (!m_impl)
        return;
    Tracenf("[LOBBY] login accepted account_id=%llu", static_cast<unsigned long long>(account_id));
    m_impl->lobbyActive = true;
    if (m_impl->loginAcceptedCallback)
        m_impl->loginAcceptedCallback();
    if (m_impl->lobbyShownCallback)
        m_impl->lobbyShownCallback();
    if (m_impl->clientSession)
        m_impl->clientSession->SendCharacterListRequest();
}
void GameClientLayer::OnLoginRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetLoginStatus(reason.empty() ? "Login failed" : reason);
}
void GameClientLayer::OnCharacterList(const std::vector<client::net::CharacterListItem>& characters)
{
    if (m_impl && m_impl->lobbyCharactersCallback)
        m_impl->lobbyCharactersCallback(characters);
}
void GameClientLayer::OnEnterWorldToken(std::vector<std::uint8_t> token, const std::string& host, std::uint16_t port)
{
    if (!m_impl || !m_impl->clientSession)
        return;
    m_impl->pendingEnterWorldToken = std::move(token);
    m_impl->pendingGameHost = host;
    m_impl->pendingGamePort = port;
    m_impl->pendingGameConnect = true;
    m_impl->pendingEnterWorldConnect = false;
    m_impl->SetLobbyStatus("Connecting to world...");
    m_impl->clientSession->Disconnect();
}
void GameClientLayer::OnEnterWorldAccepted(std::uint32_t net_id, client::net::Vec3 spawn_pos)
{
    if (!m_impl)
        return;
    m_impl->inWorld = true;
    m_impl->lobbyActive = false;
    m_impl->ownNetId = net_id;
    m_impl->entities.clear();
    WorldRenderEntity& own = m_impl->UpsertEntity(net_id);
    own.name = "You";
    own.position = spawn_pos;
    own.level = 1;
    own.hpCurrent = 1.0f;
    own.hpMax = 1.0f;
    if (m_impl->lobbyEnteredWorldCallback)
        m_impl->lobbyEnteredWorldCallback();
    Tracenf("[WORLD] enter accepted net_id=%u", net_id);
}
void GameClientLayer::OnEnterWorldRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetLobbyStatus(reason.empty() ? "Enter world failed" : reason);
}
void GameClientLayer::OnEntitySpawn(const client::net::EntitySpawnInfo& entity)
{
    if (!m_impl)
        return;
    WorldRenderEntity& renderEntity = m_impl->UpsertEntity(entity.netId);
    renderEntity.name = entity.name;
    renderEntity.position = entity.spawnPos;
    renderEntity.heading = entity.heading;
    renderEntity.moveState = client::net::MoveState::Idle;
    renderEntity.mobTypeId = entity.mobTypeId;
    renderEntity.level = entity.level == 0 ? 1 : entity.level;
    renderEntity.hpCurrent = entity.hpCurrent;
    renderEntity.hpMax = entity.hpMax <= 0.0f ? 1.0f : entity.hpMax;
    renderEntity.hpDisplayed = renderEntity.hpCurrent;
}
void GameClientLayer::OnEntityDespawn(std::uint32_t net_id)
{
    if (!m_impl)
        return;
    m_impl->entities.erase(std::remove_if(m_impl->entities.begin(), m_impl->entities.end(),
        [net_id](const WorldRenderEntity& entity) { return entity.netId == net_id; }), m_impl->entities.end());
}
void GameClientLayer::OnEntityHealthUpdate(const client::net::EntityHealthInfo& health)
{
    if (!m_impl)
        return;
    WorldRenderEntity& entity = m_impl->UpsertEntity(health.netId);
    entity.hpCurrent = health.hpCurrent;
    entity.hpMax = health.hpMax <= 0.0f ? 1.0f : health.hpMax;
    entity.hpDisplayed = entity.hpCurrent;
}
void GameClientLayer::OnEntityDeath(std::uint32_t net_id, std::uint32_t)
{
    if (WorldRenderEntity* entity = m_impl ? m_impl->FindEntity(net_id) : nullptr)
    {
        entity->hpCurrent = 0.0f;
        entity->hpDisplayed = 0.0f;
    }
}
void GameClientLayer::OnEntityTransforms(std::uint32_t, const std::vector<client::net::EntityTransform>& transforms)
{
    if (!m_impl)
        return;
    for (const auto& transform : transforms)
    {
        WorldRenderEntity& entity = m_impl->UpsertEntity(transform.netId);
        entity.position = transform.position;
        entity.heading = transform.heading;
        entity.moveState = transform.moveState;
        if (entity.name.empty())
            entity.name = transform.netId == m_impl->ownNetId ? "You" : "Entity";
    }
}

void GameClientLayer::Destroy()
{
    m_impl.reset();
}
