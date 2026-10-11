#pragma once

#include "MapEditorTypes.h"
#include "WorldCamera.h"
#include <array>
#include <span>

constexpr std::size_t kMaxFogVolumes = 32;

// Float4-only layout shared with Fog.hlsli; one buffer per view and frame in flight.
struct FogVolumeGpu
{
    float centerShape[4]{};
    float axisXExtent[4]{};
    float axisYExtent[4]{};
    float axisZExtent[4]{};
    float colorDensity[4]{};
    float fade[4]{};
};

struct FogUniforms
{
    float inverseViewProjection[16]{};
    float camera[4]{};
    float colorDensity[4]{};
    float globalParams[4]{}; // start, base height, height falloff (0 = uniform), opacity cap
    float options[4]{}; // global enabled, affect sky, local count, global max distance
    std::array<FogVolumeGpu, kMaxFogVolumes> volumes{};
};
static_assert(sizeof(FogVolumeGpu) == 96 && sizeof(FogUniforms) == 3200);

FogVolumeGpu BuildFogVolume(const MeshSceneEntity& entity);
bool BuildFogUniforms(FogUniforms& out, const WorldCamera& camera, GlobalFogSettings global,
                     std::span<const MeshSceneEntity> entities, bool respectEditorHidden = false);
float GlobalFogOpticalDepth(const FogUniforms& fog, WorldVec3 direction, float distance, bool sky = false);
float LocalFogOpticalDepth(const FogVolumeGpu& fog, WorldVec3 origin, WorldVec3 direction, float distance);
// Transmission and density-weighted colour; overlapping volumes combine independently of entity order.
struct FogEvaluation { float transmission = 1.0f; WorldVec3 color{}; };
FogEvaluation EvaluateFog(const FogUniforms& fog, WorldVec3 direction, float distance, bool sky = false);
