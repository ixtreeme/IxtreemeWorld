#pragma once

// Engine/World boundary (Phase 1: ownership + non-goals).
//
// Generic world functionality ownership established here; implementation stays in
// existing libs for Phase 1 (no terrain/water rewrite, no 100km streaming):
//   World/Transform -> libs/render/MapEditorTypes.h (MeshSceneEntity/CameraEntity)
//   World/Terrain   -> libs/render/TerrainRenderer.* + libs/terrain_editor_system/*
//   World/Water     -> libs/render/WaterBodyIO.* + shaders/Water.hlsl
//   World/Spatial   -> libs/render/SpatialIndex.* (LOCAL ONLY, see below)
//   World/Streaming -> future (not implemented)
//   World/Navigation-> future (not implemented)
//   World/Manifest  -> future WorldManifest/WorldChunk design notes only
//
// SPATIAL INDEX POLICY (binding): libs/render/SpatialIndex is for local rendering,
// editor picking, culling, and selection ONLY. It is NOT the future 100km world
// authority. Large-world streaming will be a separate system.
//
// TERRAIN/WATER SPLIT: runtime terrain/water (Engine) vs editor terrain tools
// (Editor). Dependency is Editor TerrainTools -> Engine Terrain, never reverse.
// TerrainEditorSystem must not be linked into a future shipping client.

namespace ixengine::world
{
} // namespace ixengine::world
