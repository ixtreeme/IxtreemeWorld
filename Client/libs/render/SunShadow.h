#pragma once

// The sun's shadow cascades as the mesh renderers read them. TerrainRenderer owns and draws the
// map (terrain and meshes cast into it); shaders sample it through shaders/SunShadow.hlsli.

#include "IXRHITexture.h"
#include "WorldMath.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

// Fixed scale and whole texel translation. Constructing translated min/max bounds first
// introduces cancellation that changes scale by a few ULPs and invalidates cached layers.
inline WorldMat4 MakeSunShadowProjection(float eyeX, float eyeY, float centerZ,
    float halfSize, float depthHalf, std::uint32_t resolution)
{
    WorldMat4 projection{};
    projection.m[0] = 1.0f / halfSize;
    projection.m[5] = 1.0f / halfSize;
    projection.m[10] = 1.0f / (2.0f * depthHalf);
    const float texelSize = 2.0f * halfSize / static_cast<float>(resolution);
    const float clipTexel = 2.0f / static_cast<float>(resolution);
    projection.m[12] = -std::floor(eyeX / texelSize) * clipTexel;
    projection.m[13] = -std::floor(eyeY / texelSize) * clipTexel;
    projection.m[14] = 0.5f - centerZ * projection.m[10];
    projection.m[15] = 1.0f;
    return projection;
}

struct SunShadowReceive
{
    static constexpr std::uint32_t kCascades = 4;

    // The cascade array (one D32 layer per cascade) and its depth-compare sampler. Bound in every
    // mesh draw, so they are set even while nothing casts (the map is then cleared: all lit).
    std::shared_ptr<ixrhi::IXRHITexture> texture;
    std::shared_ptr<ixrhi::IXRHISampler> sampler;
    bool enabled = false;  // the map was drawn this frame; otherwise every surface is lit
    float mapSize = 2048.0f;
    std::array<WorldMat4, kCascades> cascadeViewProj{};  // light view-projection (row vectors), finest first
    std::array<float, kCascades> depthBias{};             // in each cascade's depth units
    std::array<float, kCascades> normalOffset{};          // metres along the surface normal
};

// The shadow fields a mesh uniform block ends with (shadowCascadeViewProj[4][16], shadowParams[4],
// shadowDepthBias[4], shadowNormalOffset[4]; read by shaders/SunShadow.hlsli).
template <typename UniformBlock>
void FillSunShadowUniform(const SunShadowReceive& shadow, UniformBlock& block)
{
    for (std::uint32_t cascade = 0; cascade < SunShadowReceive::kCascades; ++cascade)
    {
        std::memcpy(block.shadowCascadeViewProj[cascade], shadow.cascadeViewProj[cascade].m,
            sizeof(block.shadowCascadeViewProj[cascade]));
        block.shadowDepthBias[cascade] = shadow.depthBias[cascade];
        block.shadowNormalOffset[cascade] = shadow.normalOffset[cascade];
    }
    block.shadowParams[0] = shadow.enabled ? 1.0f : 0.0f;
    block.shadowParams[1] = shadow.mapSize;
    block.shadowParams[2] = 0.0f;
    block.shadowParams[3] = 0.0f;
}
