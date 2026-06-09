#pragma once

#include <cmath>
#include <cstdint>

struct WorldVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct WorldMat4
{
    float m[16]{};
};

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
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline WorldVec3 WorldSub(WorldVec3 a, WorldVec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline WorldVec3 WorldScale(WorldVec3 v, float s)
{
    return {v.x * s, v.y * s, v.z * s};
}

inline float WorldDot(WorldVec3 a, WorldVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline WorldVec3 WorldCross(WorldVec3 a, WorldVec3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

inline WorldVec3 WorldNormalize(WorldVec3 v)
{
    const float length = std::sqrt(WorldDot(v, v));
    if (length <= 0.000001f)
        return {0.0f, 0.0f, 0.0f};
    return WorldScale(v, 1.0f / length);
}

inline WorldMat4 WorldIdentity()
{
    WorldMat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
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
    WorldMat4 r = WorldIdentity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline WorldMat4 WorldPerspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    WorldMat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zFar - zNear);
    r.m[11] = 1.0f;
    r.m[14] = -(zNear * zFar) / (zFar - zNear);
    return r;
}

inline WorldMat4 WorldLookAt(WorldVec3 eye, WorldVec3 target, WorldVec3 up)
{
    const WorldVec3 zAxis = WorldNormalize(WorldSub(target, eye));
    const WorldVec3 xAxis = WorldNormalize(WorldCross(up, zAxis));
    const WorldVec3 yAxis = WorldCross(zAxis, xAxis);

    WorldMat4 r = WorldIdentity();
    r.m[0] = xAxis.x;
    r.m[4] = xAxis.y;
    r.m[8] = xAxis.z;
    r.m[12] = -WorldDot(eye, xAxis);

    r.m[1] = yAxis.x;
    r.m[5] = yAxis.y;
    r.m[9] = yAxis.z;
    r.m[13] = -WorldDot(eye, yAxis);

    r.m[2] = zAxis.x;
    r.m[6] = zAxis.y;
    r.m[10] = zAxis.z;
    r.m[14] = -WorldDot(eye, zAxis);
    return r;
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
    const WorldMat4 projection = WorldPerspective(45.0f * 3.1415926535f / 180.0f, aspect, camera.nearPlane, camera.farPlane);
    camera.viewProjection = WorldMultiply(view, projection);
    return camera;
}
