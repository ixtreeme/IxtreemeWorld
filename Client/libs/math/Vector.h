#pragma once

#include "Scalar.h"
#include "Simd.h"
#include "Types.h"

#include <math.h>
#include <cmath>

namespace ixtreeme::math
{
inline bool IsFinite(Vec2 v)
{
    return IsFinite(v.x) && IsFinite(v.y);
}

inline bool IsFinite(Vec3 v)
{
    return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z);
}

inline bool IsFinite(Vec4 v)
{
    return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z) && IsFinite(v.w);
}

inline bool NearlyEqual(Vec2 a, Vec2 b, float epsilon = Epsilon)
{
    return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon);
}

inline bool NearlyEqual(Vec3 a, Vec3 b, float epsilon = Epsilon)
{
    return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon) && NearlyEqual(a.z, b.z, epsilon);
}

inline bool NearlyEqual(Vec4 a, Vec4 b, float epsilon = Epsilon)
{
    return NearlyEqual(a.x, b.x, epsilon) &&
        NearlyEqual(a.y, b.y, epsilon) &&
        NearlyEqual(a.z, b.z, epsilon) &&
        NearlyEqual(a.w, b.w, epsilon);
}

inline float Dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}

inline float Dot(Vec3 a, Vec3 b)
{
    return simd::Dot3(simd::Load(a), simd::Load(b));
}

inline float Dot(Vec4 a, Vec4 b)
{
    return simd::Dot4(simd::Load(a), simd::Load(b));
}

inline float Cross(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return simd::StoreVec3(simd::Cross3(simd::Load(a), simd::Load(b)));
}

inline float LengthSquared(Vec2 v)
{
    return Dot(v, v);
}

inline float LengthSquared(Vec3 v)
{
    return Dot(v, v);
}

inline float LengthSquared(Vec4 v)
{
    return Dot(v, v);
}

inline float Length(Vec2 v)
{
    return Sqrt(LengthSquared(v));
}

inline float Length(Vec3 v)
{
    return Sqrt(LengthSquared(v));
}

inline float Length(Vec4 v)
{
    return Sqrt(LengthSquared(v));
}

inline float DistanceSquared(Vec2 a, Vec2 b)
{
    return LengthSquared(b - a);
}

inline float DistanceSquared(Vec3 a, Vec3 b)
{
    return LengthSquared(b - a);
}

inline float DistanceSquared(Vec4 a, Vec4 b)
{
    return LengthSquared(b - a);
}

inline float Distance(Vec2 a, Vec2 b)
{
    return Length(b - a);
}

inline float Distance(Vec3 a, Vec3 b)
{
    return Length(b - a);
}

inline float Distance(Vec4 a, Vec4 b)
{
    return Length(b - a);
}

inline Vec2 Normalize(Vec2 v)
{
    const float length = Length(v);
    if (length <= Epsilon)
        return {};
    return v / length;
}

inline Vec3 Normalize(Vec3 v)
{
    return simd::StoreVec3(simd::Normalize3(simd::Load(v)));
}

inline Vec4 Normalize(Vec4 v)
{
    return simd::StoreVec4(simd::Normalize4(simd::Load(v)));
}

inline Vec2 SafeNormalize(Vec2 v, Vec2 fallback = {1.0f, 0.0f})
{
    const float length = Length(v);
    if (length <= Epsilon)
        return fallback;
    return v / length;
}

inline Vec3 SafeNormalize(Vec3 v, Vec3 fallback = {0.0f, 1.0f, 0.0f})
{
    const float length = Length(v);
    if (length <= Epsilon)
        return fallback;
    return simd::StoreVec3(simd::Div(simd::Load(v), simd::Splat(length)));
}

inline Vec4 SafeNormalize(Vec4 v, Vec4 fallback = {0.0f, 0.0f, 0.0f, 1.0f})
{
    const float length = Length(v);
    if (length <= Epsilon)
        return fallback;
    return simd::StoreVec4(simd::Div(simd::Load(v), simd::Splat(length)));
}

inline Vec2 Min(Vec2 a, Vec2 b)
{
    return {Min(a.x, b.x), Min(a.y, b.y)};
}

inline Vec3 Min(Vec3 a, Vec3 b)
{
    return simd::StoreVec3(simd::Min(simd::Load(a), simd::Load(b)));
}

inline Vec4 Min(Vec4 a, Vec4 b)
{
    return simd::StoreVec4(simd::Min(simd::Load(a), simd::Load(b)));
}

inline Vec2 Max(Vec2 a, Vec2 b)
{
    return {Max(a.x, b.x), Max(a.y, b.y)};
}

inline Vec3 Max(Vec3 a, Vec3 b)
{
    return simd::StoreVec3(simd::Max(simd::Load(a), simd::Load(b)));
}

inline Vec4 Max(Vec4 a, Vec4 b)
{
    return simd::StoreVec4(simd::Max(simd::Load(a), simd::Load(b)));
}

inline Vec2 Abs(Vec2 v)
{
    return {Abs(v.x), Abs(v.y)};
}

inline Vec3 Abs(Vec3 v)
{
    return simd::StoreVec3(simd::Abs(simd::Load(v)));
}

inline Vec4 Abs(Vec4 v)
{
    return simd::StoreVec4(simd::Abs(simd::Load(v)));
}

inline Vec2 Clamp(Vec2 value, Vec2 minValue, Vec2 maxValue)
{
    return {Clamp(value.x, minValue.x, maxValue.x), Clamp(value.y, minValue.y, maxValue.y)};
}

inline Vec3 Clamp(Vec3 value, Vec3 minValue, Vec3 maxValue)
{
    return simd::StoreVec3(simd::Min(simd::Max(simd::Load(value), simd::Load(minValue)), simd::Load(maxValue)));
}

inline Vec4 Clamp(Vec4 value, Vec4 minValue, Vec4 maxValue)
{
    return simd::StoreVec4(simd::Min(simd::Max(simd::Load(value), simd::Load(minValue)), simd::Load(maxValue)));
}

inline Vec2 Multiply(Vec2 a, Vec2 b)
{
    return {a.x * b.x, a.y * b.y};
}

inline Vec3 Multiply(Vec3 a, Vec3 b)
{
    return simd::StoreVec3(simd::Mul(simd::Load(a), simd::Load(b)));
}

inline Vec4 Multiply(Vec4 a, Vec4 b)
{
    return simd::StoreVec4(simd::Mul(simd::Load(a), simd::Load(b)));
}

inline Vec2 Divide(Vec2 a, Vec2 b)
{
    return {a.x / b.x, a.y / b.y};
}

inline Vec3 Divide(Vec3 a, Vec3 b)
{
    return simd::StoreVec3(simd::Div(simd::Load(a), simd::Load(b)));
}

inline Vec4 Divide(Vec4 a, Vec4 b)
{
    return simd::StoreVec4(simd::Div(simd::Load(a), simd::Load(b)));
}

inline Vec2 Project(Vec2 v, Vec2 onNormal)
{
    const float denom = LengthSquared(onNormal);
    if (denom <= Epsilon)
        return {};
    return onNormal * (Dot(v, onNormal) / denom);
}

inline Vec3 Project(Vec3 v, Vec3 onNormal)
{
    const float denom = LengthSquared(onNormal);
    if (denom <= Epsilon)
        return {};
    return onNormal * (Dot(v, onNormal) / denom);
}

inline Vec2 Reject(Vec2 v, Vec2 fromNormal)
{
    return v - Project(v, fromNormal);
}

inline Vec3 Reject(Vec3 v, Vec3 fromNormal)
{
    return v - Project(v, fromNormal);
}

inline Vec2 Reflect(Vec2 direction, Vec2 normal)
{
    const Vec2 n = Normalize(normal);
    return direction - n * (2.0f * Dot(direction, n));
}

inline Vec3 Reflect(Vec3 direction, Vec3 normal)
{
    const Vec3 n = Normalize(normal);
    return direction - n * (2.0f * Dot(direction, n));
}

inline Vec3 Refract(Vec3 direction, Vec3 normal, float eta)
{
    const Vec3 d = Normalize(direction);
    const Vec3 n = Normalize(normal);
    const float cosI = Clamp(Dot(d, n), -1.0f, 1.0f);
    const float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
    if (k < 0.0f)
        return {};
    return d * eta - n * (eta * cosI + Sqrt(k));
}

inline Vec2 ClampLength(Vec2 v, float maxLength)
{
    const float lengthSq = LengthSquared(v);
    if (lengthSq <= maxLength * maxLength || lengthSq <= Epsilon)
        return v;
    return v * (maxLength / Sqrt(lengthSq));
}

inline Vec3 ClampLength(Vec3 v, float maxLength)
{
    const float lengthSq = LengthSquared(v);
    if (lengthSq <= maxLength * maxLength || lengthSq <= Epsilon)
        return v;
    return v * (maxLength / Sqrt(lengthSq));
}

inline float AngleBetweenRadians(Vec2 a, Vec2 b)
{
    const float denom = Length(a) * Length(b);
    if (denom <= Epsilon)
        return 0.0f;
    return Acos(Clamp(Dot(a, b) / denom, -1.0f, 1.0f));
}

inline float AngleBetweenRadians(Vec3 a, Vec3 b)
{
    const float denom = Length(a) * Length(b);
    if (denom <= Epsilon)
        return 0.0f;
    return Acos(Clamp(Dot(a, b) / denom, -1.0f, 1.0f));
}

inline float AngleBetweenDegrees(Vec2 a, Vec2 b)
{
    return RadiansToDegrees(AngleBetweenRadians(a, b));
}

inline float AngleBetweenDegrees(Vec3 a, Vec3 b)
{
    return RadiansToDegrees(AngleBetweenRadians(a, b));
}

inline Vec2 Perpendicular(Vec2 v)
{
    return {-v.y, v.x};
}

inline Vec3 FaceForward(Vec3 normal, Vec3 incident, Vec3 referenceNormal)
{
    return Dot(referenceNormal, incident) < 0.0f ? normal : -normal;
}
}
