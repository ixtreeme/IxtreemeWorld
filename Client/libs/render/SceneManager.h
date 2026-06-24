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
    EditorCameraState editorCamera;
    std::vector<CameraEntity> cameras;
    std::uint32_t mainCameraId = 0;
    LightingState lighting;
    PhysicsSceneSettings physics;
    TerrainSceneData terrain;
    std::vector<WaterBody> waterBodies;
    std::vector<PointLight> pointLights;
    std::vector<SpotLight> spotLights;
    std::vector<MeshSceneEntity> meshEntities;
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
    void SetWindowTitleSuffix(std::string suffix);
    void SetSceneName(const std::string& name);
    void SetPhysicsSettings(const PhysicsSceneSettings& settings);

    const std::string& GetCurrentScenePath() const { return m_currentScenePath; }
    bool IsDirty() const { return m_isDirty; }
    bool HasOpenScene() const { return m_sceneOpen; }
    const std::vector<std::string>& GetRecentScenes() const { return m_recentScenes; }
    const SceneData& GetCurrentScene() const { return m_currentScene; }

    void MarkDirty();
    void UpdateWindowTitle();

private:
    SceneManager() = default;

    bool LoadSceneInternal(const std::string& path);
    bool SaveSceneInternal(const std::string& path);
    bool PromptSaveBeforeAction(const std::string& actionName);
    void UpdateRecentList(const std::string& path);
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
    std::string m_windowTitleSuffix;
};
