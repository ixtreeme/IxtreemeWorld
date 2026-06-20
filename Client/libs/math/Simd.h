#pragma once

#include "Types.h"

#include <math.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#define IX_MATH_SIMD_SSE2 1
#include <immintrin.h>
#else
#define IX_MATH_SIMD_SSE2 0
#endif

namespace ixtreeme::math::simd
{
#if IX_MATH_SIMD_SSE2
using Float4 = __m128;

inline Float4 Set(float x, float y, float z, float w)
{
    return _mm_set_ps(w, z, y, x);
}

inline Float4 Splat(float value)
{
    return _mm_set1_ps(value);
}

inline Float4 Load(const float* values)
{
    return _mm_loadu_ps(values);
}

inline Float4 Load(Vec4 v)
{
    return Set(v.x, v.y, v.z, v.w);
}

inline Float4 Load(Vec3 v, float w = 0.0f)
{
    return Set(v.x, v.y, v.z, w);
}

inline void Store(Float4 v, float* values)
{
    _mm_storeu_ps(values, v);
}

inline Vec4 StoreVec4(Float4 v)
{
    alignas(16) float values[4];
    Store(v, values);
    return {values[0], values[1], values[2], values[3]};
}

inline Vec3 StoreVec3(Float4 v)
{
    alignas(16) float values[4];
    Store(v, values);
    return {values[0], values[1], values[2]};
}

inline Float4 Add(Float4 a, Float4 b) { return _mm_add_ps(a, b); }
inline Float4 Sub(Float4 a, Float4 b) { return _mm_sub_ps(a, b); }
inline Float4 Mul(Float4 a, Float4 b) { return _mm_mul_ps(a, b); }
inline Float4 Div(Float4 a, Float4 b) { return _mm_div_ps(a, b); }
inline Float4 Min(Float4 a, Float4 b) { return _mm_min_ps(a, b); }
inline Float4 Max(Float4 a, Float4 b) { return _mm_max_ps(a, b); }
inline Float4 Sqrt(Float4 v) { return _mm_sqrt_ps(v); }

inline Float4 Abs(Float4 v)
{
    const __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
    return _mm_and_ps(v, mask);
}

inline float GetX(Float4 v)
{
    return _mm_cvtss_f32(v);
}

inline float HorizontalAdd(Float4 v)
{
    const __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
    const __m128 sums = _mm_add_ps(v, shuf);
    const __m128 shuf2 = _mm_movehl_ps(shuf, sums);
    return _mm_cvtss_f32(_mm_add_ss(sums, shuf2));
}

inline float Dot3(Float4 a, Float4 b)
{
    const __m128 mul = _mm_mul_ps(a, b);
    const __m128 mask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    return HorizontalAdd(_mm_and_ps(mul, mask));
}

inline float Dot4(Float4 a, Float4 b)
{
    return HorizontalAdd(_mm_mul_ps(a, b));
}

inline Float4 Cross3(Float4 a, Float4 b)
{
    const __m128 aYZX = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 bYZX = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 c = _mm_sub_ps(_mm_mul_ps(a, bYZX), _mm_mul_ps(aYZX, b));
    return _mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 0, 2, 1));
}

inline Float4 Normalize3(Float4 v)
{
    const float lengthSq = Dot3(v, v);
    if (lengthSq <= 0.000001f)
        return _mm_setzero_ps();
    return _mm_mul_ps(v, _mm_set1_ps(1.0f / std::sqrt(lengthSq)));
}

inline Float4 Normalize4(Float4 v)
{
    const float lengthSq = Dot4(v, v);
    if (lengthSq <= 0.000001f)
        return _mm_setzero_ps();
    return _mm_mul_ps(v, _mm_set1_ps(1.0f / std::sqrt(lengthSq)));
}

inline Float4 TransformVec4(const Mat4& m, Float4 v)
{
    const __m128 xxxx = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
    const __m128 yyyy = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
    const __m128 zzzz = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
    const __m128 wwww = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
    const __m128 col0 = Load(&m.m[0]);
    const __m128 col1 = Load(&m.m[4]);
    const __m128 col2 = Load(&m.m[8]);
    const __m128 col3 = Load(&m.m[12]);
    return _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(col0, xxxx), _mm_mul_ps(col1, yyyy)),
        _mm_add_ps(_mm_mul_ps(col2, zzzz), _mm_mul_ps(col3, wwww)));
}

inline Mat4 MultiplyMat4(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int col = 0; col < 4; ++col)
    {
        Store(TransformVec4(a, Load(&b.m[col * 4])), &result.m[col * 4]);
    }
    return result;
}
#else
struct Float4
{
    float x;
    float y;
    float z;
    float w;
};

inline Float4 Set(float x, float y, float z, float w) { return {x, y, z, w}; }
inline Float4 Splat(float value) { return {value, value, value, value}; }
inline Float4 Load(const float* values) { return {values[0], values[1], values[2], values[3]}; }
inline Float4 Load(Vec4 v) { return {v.x, v.y, v.z, v.w}; }
inline Float4 Load(Vec3 v, float w = 0.0f) { return {v.x, v.y, v.z, w}; }
inline void Store(Float4 v, float* values)
{
    values[0] = v.x;
    values[1] = v.y;
    values[2] = v.z;
    values[3] = v.w;
}

inline Vec4 StoreVec4(Float4 v) { return {v.x, v.y, v.z, v.w}; }
inline Vec3 StoreVec3(Float4 v) { return {v.x, v.y, v.z}; }
inline Float4 Add(Float4 a, Float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Float4 Sub(Float4 a, Float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Float4 Mul(Float4 a, Float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline Float4 Div(Float4 a, Float4 b) { return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}; }
inline Float4 Min(Float4 a, Float4 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w}; }
inline Float4 Max(Float4 a, Float4 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w}; }
inline Float4 Abs(Float4 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z), std::fabs(v.w)}; }
inline Float4 Sqrt(Float4 v) { return {std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z), std::sqrt(v.w)}; }
inline float GetX(Float4 v) { return v.x; }
inline float Dot3(Float4 a, Float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Dot4(Float4 a, Float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Float4 Cross3(Float4 a, Float4 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
}

inline Float4 Normalize3(Float4 v)
{
    const float lengthSq = Dot3(v, v);
    if (lengthSq <= 0.000001f)
        return {};
    const float invLength = 1.0f / std::sqrt(lengthSq);
    return {v.x * invLength, v.y * invLength, v.z * invLength, v.w * invLength};
}

inline Float4 Normalize4(Float4 v)
{
    const float lengthSq = Dot4(v, v);
    if (lengthSq <= 0.000001f)
        return {};
    const float invLength = 1.0f / std::sqrt(lengthSq);
    return {v.x * invLength, v.y * invLength, v.z * invLength, v.w * invLength};
}

inline Float4 TransformVec4(const Mat4& m, Float4 v)
{
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w};
}

inline Mat4 MultiplyMat4(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int col = 0; col < 4; ++col)
        Store(TransformVec4(a, Load(&b.m[col * 4])), &result.m[col * 4]);
    return result;
}
#endif

inline constexpr bool Enabled()
{
    return IX_MATH_SIMD_SSE2 != 0;
}
}
