#pragma once

#include "SceneManager.h"

#include <cstdint>
#include <functional>
#include <vector>

class EditorImGui;
class RuntimeSession;
class TerrainRenderer;

namespace ixrhi
{
class IXRHIDevice;
}

struct MeshSceneEntity;

class EditorSceneRuntime
{
public:
    struct Context
    {
        EditorImGui* editorImGui = nullptr;
        RuntimeSession* runtimeSession = nullptr;
        TerrainRenderer* terrain = nullptr;
        ixrhi::IXRHIDevice* rhi = nullptr;

        std::vector<WaterBody>* waterBodies = nullptr;
        std::vector<PointLight>* pointLights = nullptr;
        std::vector<SpotLight>* spotLights = nullptr;
        std::vector<MeshSceneEntity>* meshEntities = nullptr;
        // The scene sky the renderer draws (the editor's Sky panel edits its own copy).
        SkySettings* sky = nullptr;

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

        std::vector<CameraEntity>* cameras = nullptr;
        std::uint32_t* nextCameraEntityId = nullptr;
        std::uint32_t* mainCameraId = nullptr;
        std::function<EditorCameraState()> captureEditorCamera;
        std::function<void(const EditorCameraState&)> applyEditorCamera;
        // Waits for the scene's models, which rebuildStaticMeshSpatialIndex started loading (they load
        // while the terrain does), and puts their entities into the spatial index.
        std::function<void()> finishSceneModelLoads;
    };

    explicit EditorSceneRuntime(Context context);

    SceneData BuildSceneSnapshot() const;
    // Same, rebuilt into `scene` reusing its terrain grid buffers. Without the terrain grids (heights,
    // attributes, splat: megabytes) for the per-frame snapshot; a save asks for the full one.
    void BuildSceneSnapshot(SceneData& scene, bool includeTerrainGrids = true) const;
    void ApplySceneData(const SceneData& scene);

private:
    Context m_context;
};
