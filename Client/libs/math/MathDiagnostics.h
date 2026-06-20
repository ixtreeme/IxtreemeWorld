#pragma once

#include <cstdint>
#include <string>

namespace ixtreeme::math::diagnostics
{
struct SelfCheckResult
{
    bool passed = false;
    float maxAbsError = 0.0f;
    std::uint32_t checks = 0;
    std::string backend;
};

struct BenchmarkResult
{
    std::string backend;
    std::uint32_t iterations = 0;
    double columnMajorMat4Ms = 0.0;
    double rowMajorMat4Ms = 0.0;
    double transformVec4Ms = 0.0;
    double frustumAabbMs = 0.0;
    float checksum = 0.0f;
};

SelfCheckResult RunSelfCheck();
BenchmarkResult RunBenchmark(std::uint32_t iterations = 200000);
}
