#include "MathDiagnostics.h"

#include "IXMath.h"
#include "MathBatch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace ixtreeme::math::diagnostics
{
namespace
{
Mat4 MultiplyColumnMajorScalar(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            result.m[col * 4 + row] =
                a.m[0 * 4 + row] * b.m[col * 4 + 0] +
                a.m[1 * 4 + row] * b.m[col * 4 + 1] +
                a.m[2 * 4 + row] * b.m[col * 4 + 2] +
                a.m[3 * 4 + row] * b.m[col * 4 + 3];
        }
    }
    return result;
}

Mat4 MultiplyRowMajorScalar(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            result.m[row * 4 + col] =
                a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                a.m[row * 4 + 3] * b.m[3 * 4 + col];
        }
    }
    return result;
}

Vec4 TransformVec4Scalar(const Mat4& m, Vec4 v)
{
    return {
        m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w};
}

Mat4 SampleMatrix(std::uint32_t seed)
{
    Mat4 m{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const float value = static_cast<float>((seed >> 8) & 0xffffu) / 8192.0f - 4.0f;
        m.m[i] = value;
    }
    return m;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}

SelfCheckResult RunSelfCheck()
{
    SelfCheckResult result{};
    result.backend = simd::ActiveBackendName();
    float maxError = 0.0f;
    std::uint32_t checks = 0;

    auto checkFloat = [&](float actual, float expected) {
        maxError = Max(maxError, Abs(actual - expected));
        ++checks;
    };

    for (std::uint32_t i = 0; i < 16; ++i)
    {
        const Mat4 a = SampleMatrix(17u + i * 29u);
        const Mat4 b = SampleMatrix(91u + i * 31u);
        const Mat4 expectedColumn = MultiplyColumnMajorScalar(a, b);
        const Mat4 actualColumn = a * b;
        const Mat4 expectedRow = MultiplyRowMajorScalar(a, b);
        const Mat4 actualRow = MultiplyRowMajor(a, b);
        for (std::size_t e = 0; e < 16; ++e)
        {
            checkFloat(actualColumn.m[e], expectedColumn.m[e]);
            checkFloat(actualRow.m[e], expectedRow.m[e]);
        }

        const Vec4 v{
            static_cast<float>(i) * 0.17f + 1.0f,
            static_cast<float>(i) * -0.09f + 0.25f,
            static_cast<float>(i) * 0.23f - 0.5f,
            1.0f};
        const Vec4 expectedVec = TransformVec4Scalar(a, v);
        const Vec4 actualVec = a * v;
        checkFloat(actualVec.x, expectedVec.x);
        checkFloat(actualVec.y, expectedVec.y);
        checkFloat(actualVec.z, expectedVec.z);
        checkFloat(actualVec.w, expectedVec.w);
    }

    result.maxAbsError = maxError;
    result.checks = checks;
    result.passed = maxError <= 0.0005f;
    return result;
}

BenchmarkResult RunBenchmark(std::uint32_t iterations)
{
    BenchmarkResult result{};
    result.backend = simd::ActiveBackendName();
    result.iterations = std::max(1u, iterations);

    Mat4 a = TRS({1.0f, 2.0f, 3.0f}, FromEulerRadians({0.2f, 0.7f, -0.3f}), {1.2f, 0.9f, 1.4f});
    Mat4 b = TRS({-2.0f, 1.0f, 0.5f}, FromEulerRadians({-0.5f, 0.1f, 0.8f}), {0.7f, 1.3f, 1.0f});
    Mat4 accum = Mat4Identity();
    Vec4 vec{1.0f, 2.0f, -0.5f, 1.0f};
    Vec4 vecAccum{};

    auto start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < result.iterations; ++i)
    {
        accum = (i & 1u) ? (a * b) : (b * a);
        if ((i & 15u) == 0)
            a.m[12] += 0.0001f;
    }
    result.columnMajorMat4Ms = MillisecondsSince(start);

    start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < result.iterations; ++i)
    {
        accum = (i & 1u) ? MultiplyRowMajor(a, b) : MultiplyRowMajor(b, a);
        if ((i & 15u) == 0)
            b.m[3] -= 0.0001f;
    }
    result.rowMajorMat4Ms = MillisecondsSince(start);

    start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < result.iterations; ++i)
    {
        vecAccum = accum * vec;
        vec.x += 0.00001f;
    }
    result.transformVec4Ms = MillisecondsSince(start);

    constexpr std::size_t kBoxCount = 512;
    std::array<Aabb, kBoxCount> boxes{};
    std::array<std::uint8_t, kBoxCount> visible{};
    for (std::size_t i = 0; i < kBoxCount; ++i)
    {
        const float x = static_cast<float>(i % 32) * 3.0f - 48.0f;
        const float z = static_cast<float>(i / 32) * 3.0f - 24.0f;
        boxes[i] = {{x - 0.5f, -1.0f, z - 0.5f}, {x + 0.5f, 3.0f, z + 0.5f}};
    }
    const Mat4 viewProjection = PerspectiveVulkan(DegreesToRadians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f) *
        LookAt({0.0f, 8.0f, -25.0f}, {0.0f, 0.0f, 5.0f}, {0.0f, 1.0f, 0.0f});
    const Frustum frustum = ExtractFrustumVulkan(viewProjection);

    std::size_t visibleChecksum = 0;
    start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < result.iterations / 32u + 1u; ++i)
        visibleChecksum += IntersectsFrustumAabbBatch(frustum, boxes.data(), visible.data(), boxes.size());
    result.frustumAabbMs = MillisecondsSince(start);

    result.checksum = accum.m[0] + accum.m[5] + accum.m[10] + vecAccum.x + static_cast<float>(visibleChecksum & 0xffffu);
    return result;
}
}
