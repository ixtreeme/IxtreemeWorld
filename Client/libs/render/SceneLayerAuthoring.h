#pragma once

#include "SceneManager.h"
#include "map/LayerActorMovement.h"
#include "map/LayerClearance.h"
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
    // 3D-4B clearance bake over every static collider + terrain.
    mx::map::LayerClearanceReport clearance;
    std::size_t obstructionSources = 0;
    std::vector<std::string> errors;
};

// The actor class the editor bakes clearance for: the CharacterController
// component's default capsule (radius 0.35 m, total height 1.8 m, step
// 0.35 m), 0.25 m cells, 2 cm floor contact.
mx::map::LayerClearanceProfile SceneLayerClearanceProfile() noexcept;
mx::map::LayerActorProfile SceneLayerActorProfile() noexcept;

// Static obstruction geometry of one entity in package coordinates, for the
// clearance bake. `physical` is false (and the call succeeds) for entities
// that do not collide as static world: no/disabled/trigger collider, the
// NoCollision layer, character controllers, skinned or dynamic bodies. Box
// and Mesh colliders give their exact collision triangles; Sphere, Capsule
// and ConvexHull give a conservative axis-aligned box (+ Jolt's 5 cm convex
// radius). Missing mesh triangles fail instead of guessing.
bool BuildLayerObstructionMesh(const MeshSceneEntity& mesh,
    const LayerCollisionGeometryProvider& geometryProvider,
    mx::map::LayerObstructionMesh& output, bool& physical, std::string& error);

// Uses the same static collider centre/absolute scale/quaternion transform as
// the current Jolt path. Never substitutes a render bounding box for a mesh.
bool BuildLayerCollisionMesh(const MeshSceneEntity& mesh,
    const LayerCollisionGeometryProvider& geometryProvider,
    mx::map::LayerCollisionMesh& output, std::string& error);

// Opt-in model colliders and rectangular water surfaces; terrain remains the
// existing heightfield. Exact ground volumes retain the cooker's source/
// component support plane; water does not. 3D-4B: every walkable support
// volume then gets a clearance grid proven against ALL static colliders and
// the terrain, and exactly touching support rectangles within the step
// height get proven step/ramp portals. The bake is metadata and an offline
// query contract, not an activation of layered simulation.
bool GenerateSceneLayers(const SceneData& scene,
    const LayerCollisionGeometryProvider& geometryProvider,
    SceneLayerAuthoringResult& result);

// Writes one validated, fresh sidecar. Existing files are never overwritten.
bool ExportSceneLayerSidecar(const std::filesystem::path& path,
    const SceneLayerAuthoringResult& result, std::string& error);
