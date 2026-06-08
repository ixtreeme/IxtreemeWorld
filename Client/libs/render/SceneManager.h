#pragma once

#include "MapEditorTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct SceneData
{
    std::string name = "Untitled";
    std::string sceneType = "empty";
    float cameraPosition[3] = {0.0f, 50.0f, 0.0f};
    float cameraRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float cameraFov = 60.0f;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    LightingState lighting;
    std::string terrainRef;
    std::string splatRef;
    std::vector<WaterBody> waterBodies;
    std::vector<PointLight> pointLights;
    std::vector<SpotLight> spotLights;
    std::array<MapEditorPaletteSlot, 8> paletteSlots{};
    std::vector<std::string> preloadAssets;
};

class SceneManager
{
public:
    static SceneManager& Instance();

    void NewScene();
    bool LoadScene(const std::string& path);
    bool SaveScene();
    bool SaveSceneAs(const std::string& path = {});
    void CloseScene();
    void RestoreSceneSnapshot(const SceneData& scene, const std::string& path, bool dirty);

    void SetCurrentSceneSnapshot(const SceneData& scene);
    bool ConsumePendingScene(SceneData& outScene);
    void SetWindowTitleCallback(std::function<void(const std::string&)> callback);
    void SetRuntimeUiCallbacks(std::function<void()> hideAllCallback,
                               std::function<void()> showLoginCallback,
                               std::function<void()> showLobbyCallback,
                               std::function<void()> showHudCallback,
                               std::function<void()> showLoadingCallback = {});
    void SetSceneName(const std::string& name);
    void SetSceneType(const std::string& sceneType);
    void ActivateCurrentSceneType();

    const std::string& GetCurrentScenePath() const { return m_currentScenePath; }
    bool IsDirty() const { return m_isDirty; }
    bool HasOpenScene() const { return m_sceneOpen; }
    const std::vector<std::string>& GetRecentScenes() const { return m_recentScenes; }
    const SceneData& GetCurrentScene() const { return m_currentScene; }
    const std::string& GetCurrentSceneType() const { return m_currentScene.sceneType; }

    void MarkDirty();
    void UpdateWindowTitle();

private:
    SceneManager() = default;

    bool LoadSceneInternal(const std::string& path);
    bool SaveSceneInternal(const std::string& path);
    bool PromptSaveBeforeAction(const std::string& actionName);
    void UpdateRecentList(const std::string& path);
    void ActivateSceneType(const std::string& sceneType);
    std::string OpenSceneDialog() const;
    std::string SaveSceneDialog() const;

    SceneData m_currentScene;
    SceneData m_pendingScene;
    bool m_hasPendingScene = false;
    bool m_sceneOpen = false;
    std::string m_currentScenePath;
    bool m_isDirty = false;
    std::vector<std::string> m_recentScenes;
    std::function<void(const std::string&)> m_windowTitleCallback;
    std::function<void()> m_hideAllRuntimeUiCallback;
    std::function<void()> m_showLoginCallback;
    std::function<void()> m_showLobbyCallback;
    std::function<void()> m_showHudCallback;
    std::function<void()> m_showLoadingCallback;
};
