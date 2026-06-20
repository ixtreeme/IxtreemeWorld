#pragma once

#include "Quaternion.h"
#include "Scalar.h"
#include "Simd.h"
#include "Types.h"
#include "Vector.h"

#include <math.h>
#include <cmath>

namespace ixtreeme::math
{
inline float& Mat3At(Mat3& m, std::size_t row, std::size_t col)
{
    return m.m[col * 3 + row];
}

inline const float& Mat3At(const Mat3& m, std::size_t row, std::size_t col)
{
    return m.m[col * 3 + row];
}

inline float& Mat4At(Mat4& m, std::size_t row, std::size_t col)
{
    return m.m[col * 4 + row];
}

inline const float& Mat4At(const Mat4& m, std::size_t row, std::size_t col)
{
    return m.m[col * 4 + row];
}

inline Mat3 Mat3Identity()
{
    Mat3 r{};
    Mat3At(r, 0, 0) = 1.0f;
    Mat3At(r, 1, 1) = 1.0f;
    Mat3At(r, 2, 2) = 1.0f;
    return r;
}

inline Mat4 Mat4Identity()
{
    Mat4 r{};
    Mat4At(r, 0, 0) = 1.0f;
    Mat4At(r, 1, 1) = 1.0f;
    Mat4At(r, 2, 2) = 1.0f;
    Mat4At(r, 3, 3) = 1.0f;
    return r;
}

inline Mat4 Transpose(const Mat4& m)
{
    Mat4 r{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t col = 0; col < 4; ++col)
            Mat4At(r, row, col) = Mat4At(m, col, row);
    }
    return r;
}

inline Mat3 Transpose(const Mat3& m)
{
    Mat3 r{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
            Mat3At(r, row, col) = Mat3At(m, col, row);
    }
    return r;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    return simd::MultiplyMat4(a, b);
}

inline Mat4 MultiplyRowMajor(const Mat4& a, const Mat4& b)
{
    return simd::MultiplyRowMajor4x4(a, b);
}

inline Vec4 operator*(const Mat4& m, Vec4 v)
{
    return simd::TransformVec4(m, v);
}

inline Mat3 ToMat3(Quat rotation)
{
    const Quat q = Normalize(rotation);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;

    Mat3 r{};
    Mat3At(r, 0, 0) = 1.0f - 2.0f * (yy + zz);
    Mat3At(r, 0, 1) = 2.0f * (xy - wz);
    Mat3At(r, 0, 2) = 2.0f * (xz + wy);

    Mat3At(r, 1, 0) = 2.0f * (xy + wz);
    Mat3At(r, 1, 1) = 1.0f - 2.0f * (xx + zz);
    Mat3At(r, 1, 2) = 2.0f * (yz - wx);

    Mat3At(r, 2, 0) = 2.0f * (xz - wy);
    Mat3At(r, 2, 1) = 2.0f * (yz + wx);
    Mat3At(r, 2, 2) = 1.0f - 2.0f * (xx + yy);
    return r;
}

inline Mat4 ToMat4(Quat rotation)
{
    const Mat3 m3 = ToMat3(rotation);
    Mat4 r = Mat4Identity();
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
            Mat4At(r, row, col) = Mat3At(m3, row, col);
    }
    return r;
}

inline Mat4 Translation(Vec3 translation)
{
    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 3) = translation.x;
    Mat4At(r, 1, 3) = translation.y;
    Mat4At(r, 2, 3) = translation.z;
    return r;
}

inline Mat4 Scale(Vec3 scale)
{
    Mat4 r{};
    Mat4At(r, 0, 0) = scale.x;
    Mat4At(r, 1, 1) = scale.y;
    Mat4At(r, 2, 2) = scale.z;
    Mat4At(r, 3, 3) = 1.0f;
    return r;
}

inline Mat4 RotationX(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    Mat4At(r, 1, 1) = c;
    Mat4At(r, 1, 2) = -s;
    Mat4At(r, 2, 1) = s;
    Mat4At(r, 2, 2) = c;
    return r;
}

inline Mat4 RotationY(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 0) = c;
    Mat4At(r, 0, 2) = s;
    Mat4At(r, 2, 0) = -s;
    Mat4At(r, 2, 2) = c;
    return r;
}

inline Mat4 RotationZ(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 0) = c;
    Mat4At(r, 0, 1) = -s;
    Mat4At(r, 1, 0) = s;
    Mat4At(r, 1, 1) = c;
    return r;
}

inline Mat4 RotationXRowMajor(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    r.m[5] = c;
    r.m[6] = -s;
    r.m[9] = s;
    r.m[10] = c;
    return r;
}

inline Mat4 RotationYRowMajor(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    r.m[0] = c;
    r.m[2] = s;
    r.m[8] = -s;
    r.m[10] = c;
    return r;
}

inline Mat4 RotationZRowMajor(float radians)
{
    const float c = Cos(radians);
    const float s = Sin(radians);
    Mat4 r = Mat4Identity();
    r.m[0] = c;
    r.m[1] = -s;
    r.m[4] = s;
    r.m[5] = c;
    return r;
}

inline Mat4 TRS(Vec3 translation, Quat rotation, Vec3 scale)
{
    return Translation(translation) * ToMat4(rotation) * Scale(scale);
}

inline Mat4 TRS(const Transform& transform)
{
    return TRS(transform.position, transform.rotation, transform.scale);
}

inline Vec3 TransformPoint(const Mat4& m, Vec3 point)
{
    const Vec4 result = m * Vec4{point.x, point.y, point.z, 1.0f};
    if (Abs(result.w) <= Epsilon)
        return {result.x, result.y, result.z};
    return {result.x / result.w, result.y / result.w, result.z / result.w};
}

inline Vec3 TransformVector(const Mat4& m, Vec3 vector)
{
    const Vec4 result = m * Vec4{vector.x, vector.y, vector.z, 0.0f};
    return {result.x, result.y, result.z};
}

inline Vec3 TransformPointRowVector(const float* matrix, Vec3 point)
{
    return {
        point.x * matrix[0] + point.y * matrix[4] + point.z * matrix[8] + matrix[12],
        point.x * matrix[1] + point.y * matrix[5] + point.z * matrix[9] + matrix[13],
        point.x * matrix[2] + point.y * matrix[6] + point.z * matrix[10] + matrix[14]};
}

inline Vec3 TransformVectorRowVector(const float* matrix, Vec3 vector)
{
    return {
        vector.x * matrix[0] + vector.y * matrix[4] + vector.z * matrix[8],
        vector.x * matrix[1] + vector.y * matrix[5] + vector.z * matrix[9],
        vector.x * matrix[2] + vector.y * matrix[6] + vector.z * matrix[10]};
}

inline Vec3 TransformDirection(const Mat4& m, Vec3 direction)
{
    return SafeNormalize(TransformVector(m, direction), direction);
}

inline Vec3 ExtractTranslation(const Mat4& m)
{
    return {Mat4At(m, 0, 3), Mat4At(m, 1, 3), Mat4At(m, 2, 3)};
}

inline Vec3 ExtractScale(const Mat4& m)
{
    return {
        Length(Vec3{Mat4At(m, 0, 0), Mat4At(m, 1, 0), Mat4At(m, 2, 0)}),
        Length(Vec3{Mat4At(m, 0, 1), Mat4At(m, 1, 1), Mat4At(m, 2, 1)}),
        Length(Vec3{Mat4At(m, 0, 2), Mat4At(m, 1, 2), Mat4At(m, 2, 2)})};
}

inline Mat4 PerspectiveVulkan(float fovYRadians, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / Tan(fovYRadians * 0.5f);
    Mat4 r{};
    Mat4At(r, 0, 0) = f / aspect;
    Mat4At(r, 1, 1) = -f;
    Mat4At(r, 2, 2) = zFar / (zFar - zNear);
    Mat4At(r, 2, 3) = -(zNear * zFar) / (zFar - zNear);
    Mat4At(r, 3, 2) = 1.0f;
    return r;
}

inline Mat4 OrthographicVulkan(float left, float right, float bottom, float top, float zNear, float zFar)
{
    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 0) = 2.0f / (right - left);
    Mat4At(r, 1, 1) = -2.0f / (top - bottom);
    Mat4At(r, 2, 2) = 1.0f / (zFar - zNear);
    Mat4At(r, 0, 3) = -(right + left) / (right - left);
    Mat4At(r, 1, 3) = -(top + bottom) / (top - bottom);
    Mat4At(r, 2, 3) = -zNear / (zFar - zNear);
    return r;
}

inline Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 zAxis = SafeNormalize(target - eye, {0.0f, 0.0f, 1.0f});
    const Vec3 xAxis = SafeNormalize(Cross(up, zAxis), {1.0f, 0.0f, 0.0f});
    const Vec3 yAxis = Cross(zAxis, xAxis);

    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 0) = xAxis.x;
    Mat4At(r, 0, 1) = xAxis.y;
    Mat4At(r, 0, 2) = xAxis.z;
    Mat4At(r, 0, 3) = -Dot(eye, xAxis);

    Mat4At(r, 1, 0) = yAxis.x;
    Mat4At(r, 1, 1) = yAxis.y;
    Mat4At(r, 1, 2) = yAxis.z;
    Mat4At(r, 1, 3) = -Dot(eye, yAxis);

    Mat4At(r, 2, 0) = zAxis.x;
    Mat4At(r, 2, 1) = zAxis.y;
    Mat4At(r, 2, 2) = zAxis.z;
    Mat4At(r, 2, 3) = -Dot(eye, zAxis);
    return r;
}

inline Mat4 InverseAffine(const Mat4& m)
{
    const Vec3 a{Mat4At(m, 0, 0), Mat4At(m, 1, 0), Mat4At(m, 2, 0)};
    const Vec3 b{Mat4At(m, 0, 1), Mat4At(m, 1, 1), Mat4At(m, 2, 1)};
    const Vec3 c{Mat4At(m, 0, 2), Mat4At(m, 1, 2), Mat4At(m, 2, 2)};
    const Vec3 t = ExtractTranslation(m);

    const float det = Dot(a, Cross(b, c));
    if (Abs(det) <= Epsilon)
        return Mat4Identity();
    const float invDet = 1.0f / det;

    const Vec3 row0 = Cross(b, c) * invDet;
    const Vec3 row1 = Cross(c, a) * invDet;
    const Vec3 row2 = Cross(a, b) * invDet;

    Mat4 r = Mat4Identity();
    Mat4At(r, 0, 0) = row0.x;
    Mat4At(r, 0, 1) = row0.y;
    Mat4At(r, 0, 2) = row0.z;
    Mat4At(r, 0, 3) = -Dot(row0, t);

    Mat4At(r, 1, 0) = row1.x;
    Mat4At(r, 1, 1) = row1.y;
    Mat4At(r, 1, 2) = row1.z;
    Mat4At(r, 1, 3) = -Dot(row1, t);

    Mat4At(r, 2, 0) = row2.x;
    Mat4At(r, 2, 1) = row2.y;
    Mat4At(r, 2, 2) = row2.z;
    Mat4At(r, 2, 3) = -Dot(row2, t);
    return r;
}
}
