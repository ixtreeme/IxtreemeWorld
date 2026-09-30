#pragma once

#include "SceneManager.h"
#include "map/LayeredWorldGeometry.h"

#include <array>
#include <filesystem>
#include <functional>

// Scene/package coordinates use metres: engine (X,Y-up,Z) maps to package
// (X,Y,Z-up) as (x,z,y). This is independent of legacy Auriga cm/-Y conversion.
std::array<float, 3> EngineToLayerCoordinates(const std::array<float, 3>& point) noexcept;
std::array<float, 3> LayerToEngineCoordinates(const std::array<float, 3>& point) noexcept;

using LayerCollisionGeometryProvider = std::function<bool(
    const MeshSceneEntity&, std::vector<std::array<float, 3>>&, std::vector<std::uint32_t>&)>;

struct SceneLayerAuthoringResult
{
    mx::map::LayeredWorld world;
    mx::map::Rect worldBounds;
    mx::map::LayerGeometryReport geometry;
    mx::map::LayerGenerationReport generation;
    std::vector<std::string> errors;
};

// Uses the same static collider centre/absolute scale/quaternion transform as
// the current Jolt path. Never substitutes a render bounding box for a mesh.
bool BuildLayerCollisionMesh(const MeshSceneEntity& mesh,
    const LayerCollisionGeometryProvider& geometryProvider,
    mx::map::LayerCollisionMesh& output, std::string& error);

// Opt-in model colliders and rectangular water surfaces; terrain remains the
// existing heightfield. The global occupancy band is metadata, not a proof of
// character clearance or an activation of layered simulation.
bool GenerateSceneLayers(const SceneData& scene,
    const LayerCollisionGeometryProvider& geometryProvider,
    SceneLayerAuthoringResult& result);

// Writes one validated, fresh sidecar. Existing files are never overwritten.
bool ExportSceneLayerSidecar(const std::filesystem::path& path,
    const SceneLayerAuthoringResult& result, std::string& error);
