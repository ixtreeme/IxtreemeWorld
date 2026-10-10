#pragma once

#include "math/IXMath.h"

#include <cstdint>
#include <limits>
#include <span>

namespace meshquery
{
inline ixtreeme::math::Vec3 SkinPosition(ixtreeme::math::Vec3 rest, std::uint32_t packedWeights,
    std::uint32_t packedBones, std::span<const ixtreeme::math::Mat4> palette)
{
    unsigned total = 0;
    for (unsigned i = 0; i < 4; ++i)
        total += (packedWeights >> (8 * i)) & 255u;
    if (!total)
        return rest;
    ixtreeme::math::Vec3 position{};
    for (unsigned i = 0; i < 4; ++i)
    {
        const unsigned weight = (packedWeights >> (8 * i)) & 255u;
        const unsigned bone = (packedBones >> (8 * i)) & 255u;
        if (weight && bone < palette.size())
            position = position + ixtreeme::math::TransformPoint(palette[bone], rest) * (static_cast<float>(weight) / total);
    }
    return position;
}

// Keep the transformed direction unnormalized: triangle distance then remains the original
// world-ray parameter, even under nonuniform or negative object scale.
template <typename PositionAt>
bool RaycastSurface(ixtreeme::math::Ray worldRay, const ixtreeme::math::Mat4& model,
    std::span<const std::uint32_t> indices, PositionAt&& positionAt, float& outDistance)
{
    namespace xm = ixtreeme::math;
    xm::Mat4 inverse;
    if (!xm::Inverse(model, inverse))
        return false;
    const xm::Ray ray{xm::TransformPoint(inverse, worldRay.origin),
        xm::TransformVector(inverse, worldRay.direction)};
    float nearest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        xm::RaycastHit hit;
        if (xm::IntersectRayTriangle(ray, positionAt(indices[i]), positionAt(indices[i + 1]),
                positionAt(indices[i + 2]), hit) && hit.distance < nearest)
            nearest = hit.distance;
    }
    if (nearest == std::numeric_limits<float>::max())
        return false;
    outDistance = nearest;
    return true;
}
}
