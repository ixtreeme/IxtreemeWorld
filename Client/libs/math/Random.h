#pragma once

#include "Quaternion.h"
#include "Scalar.h"
#include "Types.h"
#include "Vector.h"

#include <math.h>
#include <cmath>
#include <cstdint>

namespace ixtreeme::math
{
class Random
{
public:
    explicit constexpr Random(uint64_t seed = 0x853c49e6748fea9bULL)
        : state_(seed != 0 ? seed : 0x853c49e6748fea9bULL)
    {
    }

    constexpr void SetSeed(uint64_t seed)
    {
        state_ = seed != 0 ? seed : 0x853c49e6748fea9bULL;
    }

    constexpr uint64_t State() const
    {
        return state_;
    }

    uint32_t NextU32()
    {
        uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return static_cast<uint32_t>((x * 0x2545F4914F6CDD1DULL) >> 32);
    }

    uint64_t NextU64()
    {
        const uint64_t hi = static_cast<uint64_t>(NextU32());
        const uint64_t lo = static_cast<uint64_t>(NextU32());
        return (hi << 32) | lo;
    }

    float NextFloat01()
    {
        return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f);
    }

    float Range(float minValue, float maxValue)
    {
        return Lerp(minValue, maxValue, NextFloat01());
    }

    int32_t Range(int32_t minInclusive, int32_t maxExclusive)
    {
        if (maxExclusive <= minInclusive)
            return minInclusive;
        const uint32_t span = static_cast<uint32_t>(maxExclusive - minInclusive);
        return minInclusive + static_cast<int32_t>(NextU32() % span);
    }

    bool Chance(float probability)
    {
        return NextFloat01() < Saturate(probability);
    }

    Vec2 InsideUnitCircle()
    {
        const float angle = Range(0.0f, TwoPi);
        const float radius = std::sqrt(NextFloat01());
        return {std::cos(angle) * radius, std::sin(angle) * radius};
    }

    Vec2 OnUnitCircle()
    {
        const float angle = Range(0.0f, TwoPi);
        return {std::cos(angle), std::sin(angle)};
    }

    Vec3 OnUnitSphere()
    {
        const float z = Range(-1.0f, 1.0f);
        const float angle = Range(0.0f, TwoPi);
        const float radius = std::sqrt(Max(0.0f, 1.0f - z * z));
        return {radius * std::cos(angle), z, radius * std::sin(angle)};
    }

    Vec3 InsideUnitSphere()
    {
        return OnUnitSphere() * std::cbrt(NextFloat01());
    }

    Quat Rotation()
    {
        const float u1 = NextFloat01();
        const float u2 = NextFloat01();
        const float u3 = NextFloat01();
        const float sqrt1MinusU1 = std::sqrt(1.0f - u1);
        const float sqrtU1 = std::sqrt(u1);
        return Normalize(Quat{
            sqrt1MinusU1 * std::sin(TwoPi * u2),
            sqrt1MinusU1 * std::cos(TwoPi * u2),
            sqrtU1 * std::sin(TwoPi * u3),
            sqrtU1 * std::cos(TwoPi * u3)});
    }

private:
    uint64_t state_;
};

inline uint32_t HashU32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

inline uint32_t HashCombine(uint32_t a, uint32_t b)
{
    return HashU32(a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2)));
}

inline uint32_t HashCombine(uint32_t a, uint32_t b, uint32_t c)
{
    return HashCombine(HashCombine(a, b), c);
}
}
