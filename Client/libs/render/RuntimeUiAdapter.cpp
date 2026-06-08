#include "RuntimeUiAdapter.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <filesystem>

namespace
{
std::filesystem::path ResolveRuntimeScenePath(const client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    std::filesystem::path requested(sceneAssetPath);
    if (requested.is_absolute() && std::filesystem::exists(requested))
        return requested;

    if (auto root = assets.RootPath())
    {
        const std::filesystem::path assetRelative = *root / "assets" / requested;
        if (std::filesystem::exists(assetRelative))
            return assetRelative;
        const std::filesystem::path rootRelative = *root / requested;
        if (std::filesystem::exists(rootRelative))
            return rootRelative;
        return assetRelative;
    }

    return requested;
}

bool LoadRuntimeScene(client::asset::IAssetReader& assets, const std::string& sceneAssetPath)
{
    const std::filesystem::path scenePath = ResolveRuntimeScenePath(assets, sceneAssetPath);
    Tracenf("[BOOT] runtime scene request: %s", sceneAssetPath.c_str());
    Tracenf("[SCENE] load attempt: %s", scenePath.string().c_str());
    Tracenf("[SCENE-RUNTIME] Request load: %s -> %s",
        sceneAssetPath.c_str(),
        scenePath.string().c_str());
    return SceneManager::Instance().LoadScene(scenePath.string());
}

class NullRuntimeUiAdapter final : public RuntimeUiAdapter
{
public:
    explicit NullRuntimeUiAdapter(RmlUiLayer& rmlUi) : m_rmlUi(rmlUi) {}

    void InstallSceneRouting(SceneManager& scenes) override
    {
        scenes.SetRuntimeUiCallbacks(
            [this]() { HideAll(); },
            [this]() { HideAll(); },
            [this]() { HideAll(); },
            [this]() { HideAll(); },
            [this]() { HideAll(); });
    }

    void BindRuntime(RuntimeSession&) override {}
    void SetQuitCallback(std::function<void()>) override {}
    void HideAll() override { m_rmlUi.HideAll(); }
    bool OnInput(const InputEvent&) override { return false; }
    bool IsSettingsVisible() const override { return false; }
    void HideSettings() override {}
    void ToggleInGameMenu() override {}
    void ToggleInventory() override {}
    void UpdateHud(const RmlHudData&) override {}

private:
    RmlUiLayer& m_rmlUi;
};

class AurigaRuntimeUiAdapter final : public RuntimeUiAdapter
{
public:
    AurigaRuntimeUiAdapter(RmlUiLayer& rmlUi,
                           client::asset::IAssetReader& assets,
                           std::function<bool()> isRuntimeFlowActive)
        : m_rmlUi(rmlUi)
        , m_assets(assets)
        , m_isRuntimeFlowActive(std::move(isRuntimeFlowActive))
    {
    }

    void InstallSceneRouting(SceneManager& scenes) override
    {
        scenes.SetRuntimeUiCallbacks(
            [this]() {
                Tracen("[UI-ROUTE] RmlUi HideAll() called");
                m_rmlUi.HideAll();
            },
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    Tracen("[UI-ROUTE] callback -> ShowLogin() suppressed (Edit mode)");
                    m_rmlUi.HideAll();
                    return;
                }
                Tracen("[UI-ROUTE] callback -> ShowLogin()");
                m_rmlUi.ShowLogin();
            },
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    Tracen("[UI-ROUTE] callback -> ShowLobby() suppressed (Edit mode)");
                    m_rmlUi.HideAll();
                    return;
                }
                Tracen("[UI-ROUTE] callback -> ShowLobby()");
                m_rmlUi.ShowLobby();
            },
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    Tracen("[UI-ROUTE] callback -> ShowHud() suppressed (Edit mode)");
                    m_rmlUi.HideAll();
                    return;
                }
                Tracen("[UI-ROUTE] callback -> ShowHud()");
                m_rmlUi.ShowHud();
            },
            [this]() {
                if (!IsRuntimeFlowActive())
                    m_rmlUi.HideAll();
            });
    }

    void BindRuntime(RuntimeSession& runtime) override
    {
        m_runtime = &runtime;
        m_rmlUi.SetLoginSubmitCallback([&runtime](const std::string& username,
                                                  const std::string& password,
                                                  bool remember) {
            runtime.SubmitLogin(username, password, remember);
        });
        runtime.SetLoginCallbacks(
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    m_rmlUi.HideAll();
                    return;
                }
                m_rmlUi.SetLoginError("");
                LoadRuntimeSceneOrFallback("scenes/Lobby.scene", [this]() {
                    m_rmlUi.HideLogin();
                    m_rmlUi.ShowLobby();
                });
            },
            [this, &runtime](const std::string& message) {
                if (!IsRuntimeFlowActive())
                {
                    m_rmlUi.HideAll();
                    return;
                }
                if (!runtime.IsLobbyActive() && !runtime.IsInWorld())
                    m_rmlUi.ShowLogin();
                m_rmlUi.SetLoginError(message);
            });
        m_rmlUi.SetLobbyCallbacks(
            [&runtime](std::uint64_t characterId) {
                runtime.EnterWorldWithCharacter(characterId);
            },
            [this]() {
                m_rmlUi.ShowCharacterCreation();
            },
            [this](std::uint64_t) {
                m_rmlUi.SetLobbyStatus("Character delete is not available yet");
            },
            [this, &runtime]() {
                runtime.LogoutToLogin();
                LoadRuntimeSceneOrFallback("scenes/Login.scene", [this]() {
                    m_rmlUi.HideLobby();
                    m_rmlUi.ShowLogin();
                });
            });
        runtime.SetLobbyCallbacks(
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    m_rmlUi.HideAll();
                    return;
                }
                m_rmlUi.ShowLobby();
            },
            [this](const std::vector<client::net::CharacterListItem>& characters) {
                m_rmlUi.SetLobbyCharacters(characters);
            },
            [this](const std::string& message) {
                m_rmlUi.SetLobbyStatus(message);
            },
            [this]() {
                if (!IsRuntimeFlowActive())
                {
                    m_rmlUi.HideAll();
                    return;
                }
                LoadRuntimeSceneOrFallback("scenes/World.scene", [this]() {
                    m_rmlUi.HideLobby();
                    m_rmlUi.ShowHud();
                });
            });
        m_rmlUi.SetInGameMenuCallbacks(
            []() {},
            [this, &runtime]() {
                runtime.LogoutToLogin();
                if (!IsRuntimeFlowActive())
                {
                    m_rmlUi.HideAll();
                    return;
                }
                LoadRuntimeSceneOrFallback("scenes/Login.scene", [this]() {
                    m_rmlUi.HideHud();
                    m_rmlUi.HideInventory();
                    m_rmlUi.HideSettings();
                    m_rmlUi.HideInGameMenu();
                    m_rmlUi.ShowLogin();
                });
            },
            [this]() {
                if (m_quitCallback)
                    m_quitCallback();
            });
    }

    void SetQuitCallback(std::function<void()> callback) override { m_quitCallback = std::move(callback); }
    void HideAll() override { m_rmlUi.HideAll(); }
    bool OnInput(const InputEvent& event) override { return m_rmlUi.OnInput(event); }
    bool IsSettingsVisible() const override { return m_rmlUi.IsSettingsVisible(); }
    void HideSettings() override { m_rmlUi.HideSettings(); }
    void ToggleInGameMenu() override { m_rmlUi.ToggleInGameMenu(); }
    void ToggleInventory() override { m_rmlUi.ToggleInventory(); }
    void UpdateHud(const RmlHudData& data) override { m_rmlUi.UpdateHud(data); }

private:
    bool IsRuntimeFlowActive() const
    {
        return !m_isRuntimeFlowActive || m_isRuntimeFlowActive();
    }

    void LoadRuntimeSceneOrFallback(const char* sceneAssetPath, std::function<void()> fallback)
    {
        if (!IsRuntimeFlowActive())
        {
            Tracenf("[SCENE-FLOW] suppressed in Edit mode: %s", sceneAssetPath);
            return;
        }
        if (LoadRuntimeScene(m_assets, sceneAssetPath))
        {
            if (m_runtime)
                m_runtime->OnSceneLoaded(SceneManager::Instance().GetCurrentScene());
            return;
        }
        if (fallback)
            fallback();
    }

    RmlUiLayer& m_rmlUi;
    client::asset::IAssetReader& m_assets;
    RuntimeSession* m_runtime = nullptr;
    std::function<bool()> m_isRuntimeFlowActive;
    std::function<void()> m_quitCallback;
};
}

std::unique_ptr<RuntimeUiAdapter> CreateNullRuntimeUiAdapter(RmlUiLayer& rmlUi)
{
    return std::make_unique<NullRuntimeUiAdapter>(rmlUi);
}

std::unique_ptr<RuntimeUiAdapter> CreateAurigaRuntimeUiAdapter(RmlUiLayer& rmlUi,
                                                               client::asset::IAssetReader& assets,
                                                               std::function<bool()> isRuntimeFlowActive)
{
    return std::make_unique<AurigaRuntimeUiAdapter>(rmlUi, assets, std::move(isRuntimeFlowActive));
}
