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
    return ixtreeme::math::MultiplyRowMajor(a, b);
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

inline WorldVec3 WorldForwardFromYawPitch(float yawRadians, float pitchRadians)
{
    const float cosPitch = ixtreeme::math::Cos(pitchRadians);
    return {
        ixtreeme::math::Sin(yawRadians) * cosPitch,
        ixtreeme::math::Sin(pitchRadians),
        ixtreeme::math::Cos(yawRadians) * cosPitch};
}

inline WorldVec3 WorldRightFromYaw(float yawRadians)
{
    return {ixtreeme::math::Cos(yawRadians), 0.0f, -ixtreeme::math::Sin(yawRadians)};
}

inline WorldVec3 WorldDirectionFromAzimuthElevation(float azimuthRadians, float elevationRadians)
{
    return WorldForwardFromYawPitch(azimuthRadians, elevationRadians);
}

inline WorldVec3 WorldCirclePoint(WorldVec3 center,
                                  WorldVec3 axisA,
                                  WorldVec3 axisB,
                                  float radius,
                                  float angleRadians)
{
    return center +
        axisA * (ixtreeme::math::Cos(angleRadians) * radius) +
        axisB * (ixtreeme::math::Sin(angleRadians) * radius);
}

inline float WorldYawFromDirection(WorldVec3 direction)
{
    const WorldVec3 normalized = ixtreeme::math::SafeNormalize(direction, {0.0f, 0.0f, 1.0f});
    return ixtreeme::math::Atan2(normalized.x, normalized.z);
}

inline float WorldPitchFromDirection(WorldVec3 direction)
{
    const WorldVec3 normalized = ixtreeme::math::SafeNormalize(direction, {0.0f, 0.0f, 1.0f});
    return ixtreeme::math::Asin(ixtreeme::math::Clamp(normalized.y, -1.0f, 1.0f));
}

inline WorldVec3 WorldCameraForward(const WorldCamera& camera)
{
    return ixtreeme::math::SafeNormalize(camera.target - camera.eye, {0.0f, 0.0f, 1.0f});
}

inline WorldVec3 WorldCameraRight(const WorldCamera& camera)
{
    return ixtreeme::math::SafeNormalize(
        ixtreeme::math::Cross({0.0f, 1.0f, 0.0f}, WorldCameraForward(camera)),
        {1.0f, 0.0f, 0.0f});
}

inline WorldVec3 WorldCameraUp(const WorldCamera& camera)
{
    return ixtreeme::math::SafeNormalize(
        ixtreeme::math::Cross(WorldCameraForward(camera), WorldCameraRight(camera)),
        {0.0f, 1.0f, 0.0f});
}

inline bool WorldProjectToScreen(const WorldCamera& camera,
                                 WorldVec3 world,
                                 uint32_t width,
                                 uint32_t height,
                                 float& outX,
                                 float& outY)
{
    const auto& m = camera.viewProjection.m;
    const float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8] + m[12];
    const float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9] + m[13];
    const float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];
    if (clipW <= 0.0001f)
        return false;

    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f)
        return false;

    outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

inline WorldVec3 WorldScreenRayDirection(const WorldCamera& camera,
                                         uint32_t width,
                                         uint32_t height,
                                         int mouseX,
                                         int mouseY,
                                         float fovYRadians = ixtreeme::math::DegreesToRadians(45.0f))
{
    const float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float ndcX = width != 0 ? (2.0f * static_cast<float>(mouseX) / static_cast<float>(width)) - 1.0f : 0.0f;
    const float ndcY = height != 0 ? 1.0f - (2.0f * static_cast<float>(mouseY) / static_cast<float>(height)) : 0.0f;
    const float tanHalfFov = ixtreeme::math::NearlyEqual(fovYRadians, ixtreeme::math::DegreesToRadians(45.0f))
        ? 0.41421356237f
        : ixtreeme::math::Tan(fovYRadians * 0.5f);
    const WorldVec3 forward = WorldCameraForward(camera);
    const WorldVec3 right = ixtreeme::math::SafeNormalize(
        ixtreeme::math::Cross({0.0f, 1.0f, 0.0f}, forward),
        {1.0f, 0.0f, 0.0f});
    const WorldVec3 up = ixtreeme::math::SafeNormalize(
        ixtreeme::math::Cross(forward, right),
        {0.0f, 1.0f, 0.0f});
    return ixtreeme::math::Normalize(
        forward +
        right * (ndcX * aspect * tanHalfFov) +
        up * (ndcY * tanHalfFov));
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
