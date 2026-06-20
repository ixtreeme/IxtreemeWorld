#pragma once

#include "Scalar.h"
#include "Simd.h"
#include "Types.h"
#include "Vector.h"

#include <math.h>
#include <cmath>

namespace ixtreeme::math
{
constexpr Quat QuatIdentity()
{
    return {0.0f, 0.0f, 0.0f, 1.0f};
}

constexpr Quat operator+(Quat a, Quat b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

constexpr Quat operator-(Quat a, Quat b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

constexpr Quat operator-(Quat q)
{
    return {-q.x, -q.y, -q.z, -q.w};
}

constexpr Quat operator*(Quat q, float s)
{
    return {q.x * s, q.y * s, q.z * s, q.w * s};
}

constexpr Quat operator*(float s, Quat q)
{
    return q * s;
}

constexpr Quat operator/(Quat q, float s)
{
    return {q.x / s, q.y / s, q.z / s, q.w / s};
}

inline float Dot(Quat a, Quat b)
{
    return simd::Dot4(simd::Set(a.x, a.y, a.z, a.w), simd::Set(b.x, b.y, b.z, b.w));
}

inline float LengthSquared(Quat q)
{
    return Dot(q, q);
}

inline float Length(Quat q)
{
    return std::sqrt(LengthSquared(q));
}

inline Quat Normalize(Quat q)
{
    const auto v = simd::Set(q.x, q.y, q.z, q.w);
    const float lengthSq = simd::Dot4(v, v);
    if (lengthSq <= Epsilon)
        return QuatIdentity();
    const Vec4 normalized = simd::StoreVec4(simd::Mul(v, simd::Splat(1.0f / std::sqrt(lengthSq))));
    return {normalized.x, normalized.y, normalized.z, normalized.w};
}

inline Quat SafeNormalize(Quat q, Quat fallback = QuatIdentity())
{
    const float length = Length(q);
    if (length <= Epsilon)
        return fallback;
    return q / length;
}

constexpr Quat Conjugate(Quat q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

inline Quat Inverse(Quat q)
{
    const float lengthSq = LengthSquared(q);
    if (lengthSq <= Epsilon)
        return QuatIdentity();
    return Conjugate(q) / lengthSq;
}

inline Quat operator*(Quat a, Quat b)
{
#if IX_MATH_SIMD_SSE2
    const __m128 av = simd::Set(a.x, a.y, a.z, a.w);
    const __m128 bv = simd::Set(b.x, b.y, b.z, b.w);
    const __m128 aw = _mm_shuffle_ps(av, av, _MM_SHUFFLE(3, 3, 3, 3));
    const __m128 bw = _mm_shuffle_ps(bv, bv, _MM_SHUFFLE(3, 3, 3, 3));
    const __m128 avec = _mm_and_ps(av, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
    const __m128 bvec = _mm_and_ps(bv, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
    const __m128 xyz = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(aw, bvec), _mm_mul_ps(bw, avec)),
        simd::Cross3(avec, bvec));
    const float w = a.w * b.w - simd::Dot3(avec, bvec);
    const Vec3 v = simd::StoreVec3(xyz);
    return {v.x, v.y, v.z, w};
#else
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
#endif
}

inline Vec3 Rotate(Quat rotation, Vec3 v)
{
    const Quat q = Normalize(rotation);
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = 2.0f * Cross(u, v);
    return v + q.w * t + Cross(u, t);
}

inline Quat AngleAxisRadians(float radians, Vec3 axis)
{
    const Vec3 normalizedAxis = SafeNormalize(axis);
    const float halfAngle = radians * 0.5f;
    const float s = std::sin(halfAngle);
    return Normalize(Quat{normalizedAxis.x * s, normalizedAxis.y * s, normalizedAxis.z * s, std::cos(halfAngle)});
}

inline Quat AngleAxisDegrees(float degrees, Vec3 axis)
{
    return AngleAxisRadians(DegreesToRadians(degrees), axis);
}

inline Quat FromEulerRadians(Vec3 euler)
{
    const Quat qx = AngleAxisRadians(euler.x, {1.0f, 0.0f, 0.0f});
    const Quat qy = AngleAxisRadians(euler.y, {0.0f, 1.0f, 0.0f});
    const Quat qz = AngleAxisRadians(euler.z, {0.0f, 0.0f, 1.0f});
    return Normalize(qz * qy * qx);
}

inline Quat FromEulerDegrees(Vec3 euler)
{
    return FromEulerRadians({DegreesToRadians(euler.x), DegreesToRadians(euler.y), DegreesToRadians(euler.z)});
}

inline Vec3 ToEulerRadians(Quat rotation)
{
    const Quat q = Normalize(rotation);

    const float sinrCosp = 2.0f * (q.w * q.x + q.y * q.z);
    const float cosrCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float x = std::atan2(sinrCosp, cosrCosp);

    const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    const float y = std::fabs(sinp) >= 1.0f ? std::copysign(HalfPi, sinp) : std::asin(sinp);

    const float sinyCosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosyCosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float z = std::atan2(sinyCosp, cosyCosp);

    return {x, y, z};
}

inline Vec3 ToEulerDegrees(Quat rotation)
{
    const Vec3 radians = ToEulerRadians(rotation);
    return {RadiansToDegrees(radians.x), RadiansToDegrees(radians.y), RadiansToDegrees(radians.z)};
}

inline Quat FromToRotation(Vec3 fromDirection, Vec3 toDirection)
{
    const Vec3 from = SafeNormalize(fromDirection, {0.0f, 0.0f, 1.0f});
    const Vec3 to = SafeNormalize(toDirection, {0.0f, 0.0f, 1.0f});
    const float cosTheta = Dot(from, to);

    if (cosTheta >= 1.0f - Epsilon)
        return QuatIdentity();

    if (cosTheta <= -1.0f + Epsilon)
    {
        Vec3 axis = Cross({1.0f, 0.0f, 0.0f}, from);
        if (LengthSquared(axis) <= Epsilon)
            axis = Cross({0.0f, 1.0f, 0.0f}, from);
        return AngleAxisRadians(Pi, axis);
    }

    const Vec3 axis = Cross(from, to);
    const float s = std::sqrt((1.0f + cosTheta) * 2.0f);
    const float invS = 1.0f / s;
    return Normalize(Quat{axis.x * invS, axis.y * invS, axis.z * invS, s * 0.5f});
}

inline Quat LookRotation(Vec3 forward, Vec3 up = {0.0f, 1.0f, 0.0f})
{
    const Vec3 f = SafeNormalize(forward, {0.0f, 0.0f, 1.0f});
    Vec3 r = Cross(up, f);
    if (LengthSquared(r) <= Epsilon)
        r = Cross({0.0f, 0.0f, 1.0f}, f);
    r = SafeNormalize(r, {1.0f, 0.0f, 0.0f});
    const Vec3 u = Cross(f, r);

    const float trace = r.x + u.y + f.z;
    Quat q{};
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (u.z - f.y) / s;
        q.y = (f.x - r.z) / s;
        q.z = (r.y - u.x) / s;
    }
    else if (r.x > u.y && r.x > f.z)
    {
        const float s = std::sqrt(1.0f + r.x - u.y - f.z) * 2.0f;
        q.w = (u.z - f.y) / s;
        q.x = 0.25f * s;
        q.y = (u.x + r.y) / s;
        q.z = (f.x + r.z) / s;
    }
    else if (u.y > f.z)
    {
        const float s = std::sqrt(1.0f + u.y - r.x - f.z) * 2.0f;
        q.w = (f.x - r.z) / s;
        q.x = (u.x + r.y) / s;
        q.y = 0.25f * s;
        q.z = (f.y + u.z) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + f.z - r.x - u.y) * 2.0f;
        q.w = (r.y - u.x) / s;
        q.x = (f.x + r.z) / s;
        q.y = (f.y + u.z) / s;
        q.z = 0.25f * s;
    }
    return Normalize(q);
}

inline Quat Nlerp(Quat a, Quat b, float t)
{
    if (Dot(a, b) < 0.0f)
        b = -b;
    return Normalize(Quat{
        Lerp(a.x, b.x, t),
        Lerp(a.y, b.y, t),
        Lerp(a.z, b.z, t),
        Lerp(a.w, b.w, t)});
}

inline Quat Slerp(Quat a, Quat b, float t)
{
    float cosTheta = Dot(a, b);
    if (cosTheta < 0.0f)
    {
        b = -b;
        cosTheta = -cosTheta;
    }

    if (cosTheta > 0.9995f)
        return Nlerp(a, b, t);

    cosTheta = Clamp(cosTheta, -1.0f, 1.0f);
    const float theta = std::acos(cosTheta);
    const float sinTheta = std::sin(theta);
    if (std::fabs(sinTheta) <= Epsilon)
        return Nlerp(a, b, t);

    const float weightA = std::sin((1.0f - t) * theta) / sinTheta;
    const float weightB = std::sin(t * theta) / sinTheta;
    return Normalize(Quat{
        a.x * weightA + b.x * weightB,
        a.y * weightA + b.y * weightB,
        a.z * weightA + b.z * weightB,
        a.w * weightA + b.w * weightB});
}

inline Quat SlerpClamped(Quat a, Quat b, float t)
{
    return Slerp(a, b, Saturate(t));
}
}
