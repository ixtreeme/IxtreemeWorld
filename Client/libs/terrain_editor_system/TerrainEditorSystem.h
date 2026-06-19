#pragma once

#include "MapEditorTypes.h"
#include "WorldCamera.h"

#include <cstdint>
#include <vector>

void RegenerateCircularWaterMask(WaterBody& body);
WorldVec3 WaterBodyCenter(const WaterBody& body);
float WaterBodyWidth(const WaterBody& body);
float WaterBodyDepth(const WaterBody& body);
void ApplyWaterBodyEditorStateToBody(WaterBody& body, const WaterBodyEditorState& state);
WaterBodyEditorState BuildWaterBodyEditorState(const std::vector<WaterBody>& bodies, std::uint32_t selectedId);

TerrainSceneData NormalizeTerrainCreateRequest(TerrainSceneData request);
TerrainEditorState BuildTerrainEditorState(const TerrainSceneData& terrain, bool selected);
