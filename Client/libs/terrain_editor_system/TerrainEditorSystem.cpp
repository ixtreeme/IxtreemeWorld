#include "TerrainEditorSystem.h"

#include <algorithm>
#include <string>

void RegenerateCircularWaterMask(WaterBody& body)
{
    body.maskWidth = 32;
    body.maskHeight = 32;
    body.shapeMask.assign(static_cast<std::size_t>(body.maskWidth) * body.maskHeight, 0);

    const float cx = static_cast<float>(body.maskWidth) * 0.5f;
    const float cy = static_cast<float>(body.maskHeight) * 0.5f;
    const float radius = std::min(cx, cy) * 0.95f;
    for (std::uint32_t y = 0; y < body.maskHeight; ++y)
    {
        for (std::uint32_t x = 0; x < body.maskWidth; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const bool inside = dx * dx + dy * dy <= radius * radius;
            body.shapeMask[static_cast<std::size_t>(y) * body.maskWidth + x] = inside ? 1u : 0u;
        }
    }
}

WorldVec3 WaterBodyCenter(const WaterBody& body)
{
    return {
        (body.bboxMin[0] + body.bboxMax[0]) * 0.5f,
        body.waterLevelY,
        (body.bboxMin[1] + body.bboxMax[1]) * 0.5f
    };
}

float WaterBodyWidth(const WaterBody& body)
{
    return std::max(0.0f, body.bboxMax[0] - body.bboxMin[0]);
}

float WaterBodyDepth(const WaterBody& body)
{
    return std::max(0.0f, body.bboxMax[1] - body.bboxMin[1]);
}

void ApplyWaterBodyEditorStateToBody(WaterBody& body, const WaterBodyEditorState& state)
{
    const float width = std::clamp(state.width, 1.0f, 200.0f);
    const float depth = std::clamp(state.depth, 1.0f, 200.0f);
    const bool hasValidMask = body.maskWidth > 0 && body.maskHeight > 0 &&
        body.shapeMask.size() == static_cast<std::size_t>(body.maskWidth) * body.maskHeight;
    body.name = state.name.empty() ? ("Water_" + std::to_string(body.id)) : state.name;
    body.materialId = state.materialId;
    body.waterLevelY = state.center[1];
    body.bboxMin[0] = state.center[0] - width * 0.5f;
    body.bboxMax[0] = state.center[0] + width * 0.5f;
    body.bboxMin[1] = state.center[2] - depth * 0.5f;
    body.bboxMax[1] = state.center[2] + depth * 0.5f;
    if (!hasValidMask)
        RegenerateCircularWaterMask(body);
}

WaterBodyEditorState BuildWaterBodyEditorState(const std::vector<WaterBody>& bodies, std::uint32_t selectedId)
{
    WaterBodyEditorState state{};
    state.count = static_cast<std::uint32_t>(bodies.size());
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [selectedId](const WaterBody& body) { return selectedId != 0 && body.id == selectedId; });
    if (it == bodies.end())
        return state;

    state.selected = true;
    state.id = it->id;
    state.name = it->name;
    const WorldVec3 center = WaterBodyCenter(*it);
    state.center[0] = center.x;
    state.center[1] = center.y;
    state.center[2] = center.z;
    state.width = WaterBodyWidth(*it);
    state.depth = WaterBodyDepth(*it);
    state.materialId = it->materialId;
    state.materialName = it->materialId.empty() ? "Inline Water" : it->materialId;
    state.config = it->config;
    state.config.waterLevelY = it->waterLevelY;
    return state;
}

TerrainSceneData NormalizeTerrainCreateRequest(TerrainSceneData request)
{
    request.exists = true;
    request.cellSizeMeters = std::max(0.01f, request.cellSizeMeters);
    request.cellsX = std::max(1u, request.cellsX);
    request.cellsZ = std::max(1u, request.cellsZ);
    request.widthMeters = static_cast<float>(request.cellsX) * request.cellSizeMeters;
    request.depthMeters = static_cast<float>(request.cellsZ) * request.cellSizeMeters;
    return request;
}

TerrainEditorState BuildTerrainEditorState(const TerrainSceneData& terrain, bool selected)
{
    TerrainEditorState state{};
    state.exists = terrain.exists;
    state.selected = selected && terrain.exists;
    state.name = terrain.name.empty() ? "Terrain" : terrain.name;
    state.widthMeters = terrain.widthMeters;
    state.depthMeters = terrain.depthMeters;
    state.cellSizeMeters = terrain.cellSizeMeters;
    state.cellsX = terrain.cellsX;
    state.cellsZ = terrain.cellsZ;
    state.triplanarEnabled = terrain.triplanarEnabled;
    state.triplanarSharpness = terrain.triplanarSharpness;
    state.triplanarSlopeThreshold = terrain.triplanarSlopeThreshold;
    state.triplanarSlopeTransition = terrain.triplanarSlopeTransition;
    return state;
}
