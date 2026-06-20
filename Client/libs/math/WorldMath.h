#pragma once

#include "math/Matrix.h"
#include "math/Scalar.h"
#include "math/Types.h"
#include "math/Vector.h"

#include <cstdint>

using WorldVec3 = ixtreeme::math::Vec3;
using WorldMat4 = ixtreeme::math::Mat4;

struct WorldCamera
{
    WorldVec3 eye;
    WorldVec3 target;
    WorldMat4 viewProjection;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

inline WorldVec3 WorldAdd(WorldVec3 a, WorldVec3 b)
{
    return a + b;
}

inline WorldVec3 WorldSub(WorldVec3 a, WorldVec3 b)
{
    return a - b;
}

inline WorldVec3 WorldScale(WorldVec3 v, float s)
{
    return v * s;
}

inline float WorldDot(WorldVec3 a, WorldVec3 b)
{
    return ixtreeme::math::Dot(a, b);
}

inline WorldVec3 WorldCross(WorldVec3 a, WorldVec3 b)
{
    return ixtreeme::math::Cross(a, b);
}

inline WorldVec3 WorldNormalize(WorldVec3 v)
{
    return ixtreeme::math::Normalize(v);
}

inline WorldMat4 WorldIdentity()
{
    return ixtreeme::math::Mat4Identity();
}

inline WorldMat4 WorldMultiply(const WorldMat4& a, const WorldMat4& b)
{
    WorldMat4 r{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
        }
    }
    return r;
}

inline WorldMat4 WorldTranslation(float x, float y, float z)
{
    return ixtreeme::math::Translation({x, y, z});
}

inline WorldMat4 WorldPerspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    return ixtreeme::math::PerspectiveVulkan(fovYRadians, aspect, zNear, zFar);
}

inline WorldMat4 WorldLookAt(WorldVec3 eye, WorldVec3 target, WorldVec3 up)
{
    return ixtreeme::math::LookAt(eye, target, up);
}

inline WorldVec3 RawServerToDisplay(int32_t x, int32_t y, int32_t z)
{
    return {
        static_cast<float>(x) * 0.01f,
        static_cast<float>(z) * 0.01f,
        -static_cast<float>(y) * 0.01f};
}

inline WorldCamera BuildSpawnCamera(uint32_t width, uint32_t height)
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    WorldCamera camera{};
    camera.target = {0.0f, 1.1f, 0.0f};
    camera.eye = {0.0f, 10.0f, -20.0f};
    const WorldMat4 view = WorldLookAt(camera.eye, camera.target, {0.0f, 1.0f, 0.0f});
    camera.nearPlane = 0.1f;
    camera.farPlane = 1000.0f;
    const WorldMat4 projection = WorldPerspective(ixtreeme::math::DegreesToRadians(45.0f), aspect, camera.nearPlane, camera.farPlane);
    camera.viewProjection = WorldMultiply(view, projection);
    return camera;
}
