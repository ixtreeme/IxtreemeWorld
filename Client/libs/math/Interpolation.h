#pragma once

#include "Scalar.h"
#include "Types.h"
#include "Vector.h"
#include "Quaternion.h"

#include <math.h>
#include <cmath>
#include <limits>

namespace ixtreeme::math
{
inline float LerpClamped(float a, float b, float t)
{
    return Lerp(a, b, Saturate(t));
}

inline float SmoothLerp(float a, float b, float t)
{
    return Lerp(a, b, SmoothStep(0.0f, 1.0f, t));
}

inline float SmootherLerp(float a, float b, float t)
{
    return Lerp(a, b, SmootherStep(0.0f, 1.0f, t));
}

inline float AngleLerpDegrees(float current, float target, float t)
{
    return current + DeltaAngleDegrees(current, target) * t;
}

inline float AngleLerpDegreesClamped(float current, float target, float t)
{
    return AngleLerpDegrees(current, target, Saturate(t));
}

inline float AngleMoveTowardsDegrees(float current, float target, float maxDelta)
{
    const float delta = DeltaAngleDegrees(current, target);
    if (Abs(delta) <= maxDelta)
        return target;
    return current + static_cast<float>(Sign(delta)) * maxDelta;
}

inline Vec2 Lerp(Vec2 a, Vec2 b, float t)
{
    return a + (b - a) * t;
}

inline Vec3 Lerp(Vec3 a, Vec3 b, float t)
{
    return a + (b - a) * t;
}

inline Vec4 Lerp(Vec4 a, Vec4 b, float t)
{
    return a + (b - a) * t;
}

inline Vec2 LerpClamped(Vec2 a, Vec2 b, float t)
{
    return Lerp(a, b, Saturate(t));
}

inline Vec3 LerpClamped(Vec3 a, Vec3 b, float t)
{
    return Lerp(a, b, Saturate(t));
}

inline Vec4 LerpClamped(Vec4 a, Vec4 b, float t)
{
    return Lerp(a, b, Saturate(t));
}

inline Vec2 SmoothLerp(Vec2 a, Vec2 b, float t)
{
    return Lerp(a, b, SmoothStep(0.0f, 1.0f, t));
}

inline Vec3 SmoothLerp(Vec3 a, Vec3 b, float t)
{
    return Lerp(a, b, SmoothStep(0.0f, 1.0f, t));
}

inline Vec4 SmoothLerp(Vec4 a, Vec4 b, float t)
{
    return Lerp(a, b, SmoothStep(0.0f, 1.0f, t));
}

inline Vec2 SmootherLerp(Vec2 a, Vec2 b, float t)
{
    return Lerp(a, b, SmootherStep(0.0f, 1.0f, t));
}

inline Vec3 SmootherLerp(Vec3 a, Vec3 b, float t)
{
    return Lerp(a, b, SmootherStep(0.0f, 1.0f, t));
}

inline Vec4 SmootherLerp(Vec4 a, Vec4 b, float t)
{
    return Lerp(a, b, SmootherStep(0.0f, 1.0f, t));
}

inline Vec2 MoveTowards(Vec2 current, Vec2 target, float maxDistanceDelta)
{
    const Vec2 delta = target - current;
    const float distance = Length(delta);
    if (distance <= maxDistanceDelta || distance <= Epsilon)
        return target;
    return current + delta * (maxDistanceDelta / distance);
}

inline Vec3 MoveTowards(Vec3 current, Vec3 target, float maxDistanceDelta)
{
    const Vec3 delta = target - current;
    const float distance = Length(delta);
    if (distance <= maxDistanceDelta || distance <= Epsilon)
        return target;
    return current + delta * (maxDistanceDelta / distance);
}

inline Vec4 MoveTowards(Vec4 current, Vec4 target, float maxDistanceDelta)
{
    const Vec4 delta = target - current;
    const float distance = Length(delta);
    if (distance <= maxDistanceDelta || distance <= Epsilon)
        return target;
    return current + delta * (maxDistanceDelta / distance);
}

inline float Damp(float current, float target, float damping, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return current;
    const float t = 1.0f - Exp(-Max(0.0f, damping) * deltaTime);
    return Lerp(current, target, t);
}

inline Vec2 Damp(Vec2 current, Vec2 target, float damping, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return current;
    const float t = 1.0f - Exp(-Max(0.0f, damping) * deltaTime);
    return Lerp(current, target, t);
}

inline Vec3 Damp(Vec3 current, Vec3 target, float damping, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return current;
    const float t = 1.0f - Exp(-Max(0.0f, damping) * deltaTime);
    return Lerp(current, target, t);
}

inline Vec4 Damp(Vec4 current, Vec4 target, float damping, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return current;
    const float t = 1.0f - Exp(-Max(0.0f, damping) * deltaTime);
    return Lerp(current, target, t);
}

inline float SmoothDamp(float current,
                        float target,
                        float& currentVelocity,
                        float smoothTime,
                        float maxSpeed,
                        float deltaTime)
{
    if (deltaTime <= 0.0f)
        return current;

    smoothTime = Max(Epsilon, smoothTime);
    const float omega = 2.0f / smoothTime;
    const float x = omega * deltaTime;
    const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float originalTarget = target;

    float change = current - target;
    const float maxChange = maxSpeed * smoothTime;
    change = Clamp(change, -maxChange, maxChange);
    target = current - change;

    const float temp = (currentVelocity + omega * change) * deltaTime;
    currentVelocity = (currentVelocity - omega * temp) * exp;
    float output = target + (change + temp) * exp;

    if ((originalTarget - current > 0.0f) == (output > originalTarget))
    {
        output = originalTarget;
        currentVelocity = 0.0f;
    }
    return output;
}

inline float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float deltaTime)
{
    return SmoothDamp(current,
        target,
        currentVelocity,
        smoothTime,
        std::numeric_limits<float>::infinity(),
        deltaTime);
}

inline Vec3 SmoothDamp(Vec3 current,
                       Vec3 target,
                       Vec3& currentVelocity,
                       float smoothTime,
                       float maxSpeed,
                       float deltaTime)
{
    return {
        SmoothDamp(current.x, target.x, currentVelocity.x, smoothTime, maxSpeed, deltaTime),
        SmoothDamp(current.y, target.y, currentVelocity.y, smoothTime, maxSpeed, deltaTime),
        SmoothDamp(current.z, target.z, currentVelocity.z, smoothTime, maxSpeed, deltaTime)};
}

inline Vec3 SmoothDamp(Vec3 current, Vec3 target, Vec3& currentVelocity, float smoothTime, float deltaTime)
{
    return SmoothDamp(current,
        target,
        currentVelocity,
        smoothTime,
        std::numeric_limits<float>::infinity(),
        deltaTime);
}

}
