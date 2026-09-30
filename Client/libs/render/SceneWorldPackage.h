#pragma once

#include "SceneLayerAuthoring.h"

#include <filesystem>
#include <string>
#include <vector>

struct SceneWorldExportOptions
{
    std::string worldId;
    // Explicit authored spawn in engine world X/Z metres; no search, nearest
    // point, guessed character identity, or ground-at-zero fallback.
    float spawnX = 0.0f;
    float spawnZ = 0.0f;
};

struct SceneWorldPackageResult
{
    std::vector<std::string> errors;
    std::vector<std::string> files; // package-relative names, set after publication
    mx::map::Rect worldBounds;
    SceneLayerAuthoringResult layers;
    // Independent diagnostics: vertex height quantization and horizontal
    // float-engine versus double-package vertex placement. Neither measures
    // total query error or guarantees full 3D polygon collision parity.
    double maxHeightErrorMeters = 0.0;
    double maxGridPositionErrorMeters = 0.0;
};

inline constexpr std::size_t kMaxSceneExportTerrainVertices = 16000000;

// Writes and full/strict-validates a complete server v3 package using the
// engine's triangle terrain interpolation and canonical centred origin.
// Both the final directory and its ".pending" staging directory must be new.
// Existing directories/files are never replaced; failed owned staging is
// removed, and no partial package is published as final.
bool ExportSceneServerWorld(const std::filesystem::path& path,
    const SceneData& scene,
    const LayerCollisionGeometryProvider& geometryProvider,
    const SceneWorldExportOptions& options,
    SceneWorldPackageResult& result);
