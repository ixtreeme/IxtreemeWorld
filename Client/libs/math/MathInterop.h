#pragma once

#include "Matrix.h"
#include "Types.h"

#include <cstddef>
#include <cmath>

#if __has_include(<assimp/matrix4x4.h>) && __has_include(<assimp/vector3.h>)
#include <assimp/matrix4x4.h>
#include <assimp/vector3.h>
#define IX_MATH_INTEROP_HAS_ASSIMP 1
#else
#define IX_MATH_INTEROP_HAS_ASSIMP 0
#endif

#if __has_include(<imgui.h>)
#include <imgui.h>
#define IX_MATH_INTEROP_HAS_IMGUI 1
#else
#define IX_MATH_INTEROP_HAS_IMGUI 0
#endif

#if __has_include(<ozz/base/maths/simd_math.h>)
#include <ozz/base/maths/simd_math.h>
#define IX_MATH_INTEROP_HAS_OZZ 1
#else
#define IX_MATH_INTEROP_HAS_OZZ 0
#endif

namespace ixtreeme::math::interop
{
inline const float* Data(const Mat4& matrix)
{
    return matrix.m;
}

inline float* Data(Mat4& matrix)
{
    return matrix.m;
}

inline Mat4 FromRowMajor4x4(const float* values)
{
    Mat4 out{};
    for (std::size_t i = 0; i < 16; ++i)
        out.m[i] = values[i];
    return out;
}

inline void StoreRowMajor4x4(const Mat4& matrix, float* values)
{
    for (std::size_t i = 0; i < 16; ++i)
        values[i] = matrix.m[i];
}

inline Mat4 FromColumnMajor4x4(const float* values)
{
    Mat4 out{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t col = 0; col < 4; ++col)
            out.m[row * 4 + col] = values[col * 4 + row];
    }
    return out;
}

inline void StoreColumnMajor4x4(const Mat4& matrix, float* values)
{
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t col = 0; col < 4; ++col)
            values[col * 4 + row] = matrix.m[row * 4 + col];
    }
}

inline Vec2 FromFloat2(const float* values)
{
    return {values[0], values[1]};
}

inline Vec3 FromFloat3(const float* values)
{
    return {values[0], values[1], values[2]};
}

inline Vec4 FromFloat4(const float* values)
{
    return {values[0], values[1], values[2], values[3]};
}

inline void StoreFloat2(Vec2 value, float* values)
{
    values[0] = value.x;
    values[1] = value.y;
}

inline void StoreFloat3(Vec3 value, float* values)
{
    values[0] = value.x;
    values[1] = value.y;
    values[2] = value.z;
}

inline void StoreFloat4(Vec4 value, float* values)
{
    values[0] = value.x;
    values[1] = value.y;
    values[2] = value.z;
    values[3] = value.w;
}

inline void NormalizeFloat3(float* values)
{
    const float lengthSq = values[0] * values[0] + values[1] * values[1] + values[2] * values[2];
    if (lengthSq <= Epsilon * Epsilon)
        return;
    const float invLength = 1.0f / Sqrt(lengthSq);
    values[0] *= invLength;
    values[1] *= invLength;
    values[2] *= invLength;
}

inline void TransformPointRowVector(const float* matrix, const float* in, float* out)
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8] + matrix[12];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9] + matrix[13];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10] + matrix[14];
}

inline void TransformVectorRowVector(const float* matrix, const float* in, float* out)
{
    out[0] = in[0] * matrix[0] + in[1] * matrix[4] + in[2] * matrix[8];
    out[1] = in[0] * matrix[1] + in[1] * matrix[5] + in[2] * matrix[9];
    out[2] = in[0] * matrix[2] + in[1] * matrix[6] + in[2] * matrix[10];
}

#if IX_MATH_INTEROP_HAS_ASSIMP
inline Vec3 FromAssimp(const aiVector3D& value)
{
    return {value.x, value.y, value.z};
}

inline aiVector3D ToAssimp(Vec3 value)
{
    return aiVector3D(value.x, value.y, value.z);
}

inline Mat4 FromAssimpRowMajor(const aiMatrix4x4& matrix)
{
    return Mat4{{matrix.a1, matrix.a2, matrix.a3, matrix.a4,
        matrix.b1, matrix.b2, matrix.b3, matrix.b4,
        matrix.c1, matrix.c2, matrix.c3, matrix.c4,
        matrix.d1, matrix.d2, matrix.d3, matrix.d4}};
}

inline aiMatrix4x4 ToAssimpRowMajor(const Mat4& matrix)
{
    return aiMatrix4x4(
        matrix.m[0], matrix.m[1], matrix.m[2], matrix.m[3],
        matrix.m[4], matrix.m[5], matrix.m[6], matrix.m[7],
        matrix.m[8], matrix.m[9], matrix.m[10], matrix.m[11],
        matrix.m[12], matrix.m[13], matrix.m[14], matrix.m[15]);
}

inline aiMatrix4x4 ToAssimpRowMajor(const float* values)
{
    return ToAssimpRowMajor(FromRowMajor4x4(values));
}
#endif

#if IX_MATH_INTEROP_HAS_IMGUI
inline Vec2 FromImGui(ImVec2 value)
{
    return {value.x, value.y};
}

inline Vec4 FromImGui(ImVec4 value)
{
    return {value.x, value.y, value.z, value.w};
}

inline ImVec2 ToImGui(Vec2 value)
{
    return ImVec2(value.x, value.y);
}

inline ImVec4 ToImGui(Vec4 value)
{
    return ImVec4(value.x, value.y, value.z, value.w);
}
#endif

#if IX_MATH_INTEROP_HAS_OZZ
inline Mat4 FromOzzFloat4x4(const ozz::math::Float4x4& matrix)
{
    float columns[4][4]{};
    for (int col = 0; col < 4; ++col)
        ozz::math::StorePtrU(matrix.cols[col], columns[col]);

    Mat4 out{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            out.m[col * 4 + row] = columns[col][row];
    }
    return out;
}

inline Mat4 FromOzzFloat4x4RowMajor(const ozz::math::Float4x4& matrix)
{
    float columns[4][4]{};
    for (int col = 0; col < 4; ++col)
        ozz::math::StorePtrU(matrix.cols[col], columns[col]);

    Mat4 out{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            out.m[row * 4 + col] = columns[col][row];
    }
    return out;
}

inline Mat4 FromOzzFloat4x4RowVectorPalette(const ozz::math::Float4x4& matrix)
{
    float columns[4][4]{};
    for (int col = 0; col < 4; ++col)
        ozz::math::StorePtrU(matrix.cols[col], columns[col]);

    Mat4 out{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
            out.m[row * 4 + col] = columns[row][col];
    }
    return out;
}
#endif
}
