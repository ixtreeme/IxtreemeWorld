#pragma once
#include "math/Matrix.h"

// The static renderer and geometry baking must use the same Euler convention.
inline ixtreeme::math::Mat4 MeshModelMatrix(const float* position, const float* rotation, const float* scale)
{
    namespace xm = ixtreeme::math;
    return xm::MultiplyRowMajor(xm::MultiplyRowMajor(xm::MultiplyRowMajor(
        xm::MultiplyRowMajor(xm::Scale({scale[0], scale[1], scale[2]}), xm::RotationX(rotation[0])),
        xm::RotationYRowMajor(rotation[1])), xm::RotationZ(rotation[2])),
        xm::Translation({position[0], position[1], position[2]}));
}
