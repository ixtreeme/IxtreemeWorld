#pragma once

#include "Geometry.h"
#include "Matrix.h"

#include <cstddef>
#include <cstdint>

namespace ixtreeme::math
{
inline void TransformAabbBatch(const Aabb* boxes, const Mat4* transforms, Aabb* out, std::size_t count)
{
    if (!boxes || !transforms || !out)
        return;
    for (std::size_t i = 0; i < count; ++i)
        out[i] = TransformAabb(boxes[i], transforms[i]);
}

inline std::size_t IntersectsFrustumAabbBatch(const Frustum& frustum,
                                              const Aabb* boxes,
                                              std::uint8_t* visibility,
                                              std::size_t count)
{
    if (!boxes || !visibility)
        return 0;

    std::size_t visibleCount = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const bool visible = Intersects(frustum, boxes[i]);
        visibility[i] = visible ? 1u : 0u;
        visibleCount += visible ? 1u : 0u;
    }
    return visibleCount;
}
}
