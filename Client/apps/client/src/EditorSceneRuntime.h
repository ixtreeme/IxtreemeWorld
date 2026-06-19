#pragma once

#include "SceneManager.h"

#include <cstdint>
#include <functional>
#include <vector>

class EditorImGui;
class RuntimeSession;
class TerrainRenderer;
class VulkanDevice;

struct MeshSceneEntity;

class EditorSceneRuntime
{
public:
    struct Context
    {
        EditorImGui* editorImGui = nullptr;
        RuntimeSession* runtimeSession = nullptr;
        TerrainRenderer* terrain = nullptr;
        VulkanDevice* device = nullptr;

        std::vector<WaterBody>* waterBodies = nullptr;
        std::vector<PointLight>* pointLights = nullptr;
        std::vector<SpotLight>* spotLights = nullptr;
        std::vector<MeshSceneEntity>* meshEntities = nullptr;

        bool* waterBodiesDirty = nullptr;
        bool terrainOk = false;

        std::uint32_t* nextWaterBodyId = nullptr;
        std::uint32_t* nextLightId = nullptr;
        std::uint32_t* nextMeshEntityId = nullptr;

        std::function<void()> clearSelection;
        std::function<void()> resetHierarchyEntities;
        std::function<void()> rebuildStaticMeshSpatialIndex;
        std::function<void()> syncTerrainAssetRoots;
        std::function<void(MeshSceneEntity&)> ensureMeshMaterialSlots;
    };

    explicit EditorSceneRuntime(Context context);

    SceneData BuildSceneSnapshot() const;
    void ApplySceneData(const SceneData& scene);

private:
    Context m_context;
};
