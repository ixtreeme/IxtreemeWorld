#include "EditorSceneRuntime.h"

#include "Debug.h"
#include "EditorImGui.h"
#include "RuntimeSession.h"
#include "TerrainRenderer.h"
#include "VulkanDevice.h"

#include <algorithm>
#include <utility>

EditorSceneRuntime::EditorSceneRuntime(Context context)
    : m_context(std::move(context))
{
}

SceneData EditorSceneRuntime::BuildSceneSnapshot() const
{
    SceneData scene;
    if (m_context.editorImGui)
        scene.lighting = m_context.editorImGui->GetLightingState();
    if (m_context.waterBodies)
        scene.waterBodies = *m_context.waterBodies;
    if (m_context.pointLights)
        scene.pointLights = *m_context.pointLights;
    if (m_context.spotLights)
        scene.spotLights = *m_context.spotLights;
    if (m_context.meshEntities)
        scene.meshEntities = *m_context.meshEntities;

    if (m_context.terrainOk && m_context.terrain)
    {
        scene.terrain = m_context.terrain->GetTerrainSceneData();
        scene.paletteSlots = m_context.terrain->GetPaletteSlots();
    }
    else if (m_context.runtimeSession)
    {
        scene.paletteSlots = m_context.runtimeSession->GetPaletteSlots();
    }
    return scene;
}

void EditorSceneRuntime::ApplySceneData(const SceneData& scene)
{
    if (m_context.waterBodies)
        *m_context.waterBodies = scene.waterBodies;
    if (m_context.pointLights)
        *m_context.pointLights = scene.pointLights;
    if (m_context.spotLights)
        *m_context.spotLights = scene.spotLights;
    if (m_context.meshEntities)
    {
        *m_context.meshEntities = scene.meshEntities;
        if (m_context.ensureMeshMaterialSlots)
        {
            for (MeshSceneEntity& mesh : *m_context.meshEntities)
                m_context.ensureMeshMaterialSlots(mesh);
        }
    }

    if (m_context.clearSelection)
        m_context.clearSelection();
    if (m_context.resetHierarchyEntities)
        m_context.resetHierarchyEntities();

    if (m_context.nextWaterBodyId && m_context.waterBodies)
    {
        *m_context.nextWaterBodyId = 1;
        for (const WaterBody& body : *m_context.waterBodies)
            *m_context.nextWaterBodyId = std::max(*m_context.nextWaterBodyId, body.id + 1u);
    }
    if (m_context.nextLightId)
    {
        *m_context.nextLightId = 1;
        if (m_context.pointLights)
        {
            for (const PointLight& light : *m_context.pointLights)
                *m_context.nextLightId = std::max(*m_context.nextLightId, light.id + 1u);
        }
        if (m_context.spotLights)
        {
            for (const SpotLight& light : *m_context.spotLights)
                *m_context.nextLightId = std::max(*m_context.nextLightId, light.id + 1u);
        }
    }
    if (m_context.nextMeshEntityId && m_context.meshEntities)
    {
        *m_context.nextMeshEntityId = 1;
        for (const MeshSceneEntity& mesh : *m_context.meshEntities)
            *m_context.nextMeshEntityId = std::max(*m_context.nextMeshEntityId, mesh.id + 1u);
    }

    if (m_context.rebuildStaticMeshSpatialIndex)
        m_context.rebuildStaticMeshSpatialIndex();

    if (m_context.editorImGui)
        m_context.editorImGui->SetLightingState(scene.lighting);
    if (m_context.runtimeSession)
    {
        m_context.runtimeSession->SetDynamicLightEditorState({});
        m_context.runtimeSession->SetWaterBodyEditorState({});
    }

    if (m_context.terrainOk && m_context.terrain && m_context.device)
    {
        if (m_context.syncTerrainAssetRoots)
            m_context.syncTerrainAssetRoots();

        m_context.terrain->SetLightingState(scene.lighting);
        if (scene.terrain.exists)
        {
            m_context.terrain->CreateFlatTerrain(*m_context.device, scene.terrain);
            if (m_context.waterBodies)
            {
                m_context.terrain->SetWaterBodies(*m_context.device, *m_context.waterBodies);
                *m_context.waterBodies = m_context.terrain->GetWaterBodies();
            }
        }
        else
        {
            m_context.terrain->ClearTerrain(*m_context.device);
            if (m_context.waterBodies)
                m_context.waterBodies->clear();
            Tracen("[SCENE] no terrain in scene");
        }
        if (m_context.terrain->ApplyPaletteSlots(*m_context.device, scene.paletteSlots) &&
            m_context.editorImGui)
        {
            m_context.editorImGui->SetPaletteSlots(m_context.terrain->GetPaletteSlots());
        }
    }

    if (m_context.waterBodiesDirty)
        *m_context.waterBodiesDirty = false;

    Tracenf("[SCENE] Applied editor scene state: terrain=%s water=%zu point=%zu spot=%zu mesh=%zu",
        (m_context.terrainOk && m_context.terrain && m_context.terrain->HasTerrain()) ? "yes" : "no",
        m_context.waterBodies ? m_context.waterBodies->size() : 0u,
        m_context.pointLights ? m_context.pointLights->size() : 0u,
        m_context.spotLights ? m_context.spotLights->size() : 0u,
        m_context.meshEntities ? m_context.meshEntities->size() : 0u);
}
