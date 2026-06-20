#pragma once

#include "Random.h"
#include "Scalar.h"
#include "Types.h"

#include <math.h>
#include <cmath>
#include <cstdint>

namespace ixtreeme::math::noise
{
inline uint32_t CoordHash1D(int32_t x, int32_t seed = 0)
{
    return HashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(seed));
}

inline uint32_t CoordHash2D(int32_t x, int32_t y, int32_t seed = 0)
{
    return HashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(seed));
}

inline uint32_t CoordHash3D(int32_t x, int32_t y, int32_t z, int32_t seed = 0)
{
    return HashCombine(CoordHash2D(x, y, seed), static_cast<uint32_t>(z));
}

inline float HashToUnitFloat(uint32_t hash)
{
    return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
}

inline float Value1D(int32_t x, int32_t seed = 0)
{
    return HashToUnitFloat(CoordHash1D(x, seed));
}

inline float Value2D(int32_t x, int32_t y, int32_t seed = 0)
{
    return HashToUnitFloat(CoordHash2D(x, y, seed));
}

inline float Value3D(int32_t x, int32_t y, int32_t z, int32_t seed = 0)
{
    return HashToUnitFloat(CoordHash3D(x, y, z, seed));
}

inline float SignedValue1D(int32_t x, int32_t seed = 0)
{
    return Value1D(x, seed) * 2.0f - 1.0f;
}

inline float SignedValue2D(int32_t x, int32_t y, int32_t seed = 0)
{
    return Value2D(x, y, seed) * 2.0f - 1.0f;
}

inline float SignedValue3D(int32_t x, int32_t y, int32_t z, int32_t seed = 0)
{
    return Value3D(x, y, z, seed) * 2.0f - 1.0f;
}

inline float Fade(float t)
{
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float ValueNoise1D(float x, int32_t seed = 0)
{
    const int32_t x0 = static_cast<int32_t>(std::floor(x));
    const int32_t x1 = x0 + 1;
    const float tx = Fade(x - static_cast<float>(x0));
    return Lerp(Value1D(x0, seed), Value1D(x1, seed), tx);
}

inline float ValueNoise2D(Vec2 p, int32_t seed = 0)
{
    const int32_t x0 = static_cast<int32_t>(std::floor(p.x));
    const int32_t y0 = static_cast<int32_t>(std::floor(p.y));
    const int32_t x1 = x0 + 1;
    const int32_t y1 = y0 + 1;
    const float tx = Fade(p.x - static_cast<float>(x0));
    const float ty = Fade(p.y - static_cast<float>(y0));

    const float a = Lerp(Value2D(x0, y0, seed), Value2D(x1, y0, seed), tx);
    const float b = Lerp(Value2D(x0, y1, seed), Value2D(x1, y1, seed), tx);
    return Lerp(a, b, ty);
}

inline float ValueNoise3D(Vec3 p, int32_t seed = 0)
{
    const int32_t x0 = static_cast<int32_t>(std::floor(p.x));
    const int32_t y0 = static_cast<int32_t>(std::floor(p.y));
    const int32_t z0 = static_cast<int32_t>(std::floor(p.z));
    const int32_t x1 = x0 + 1;
    const int32_t y1 = y0 + 1;
    const int32_t z1 = z0 + 1;
    const float tx = Fade(p.x - static_cast<float>(x0));
    const float ty = Fade(p.y - static_cast<float>(y0));
    const float tz = Fade(p.z - static_cast<float>(z0));

    const float x00 = Lerp(Value3D(x0, y0, z0, seed), Value3D(x1, y0, z0, seed), tx);
    const float x10 = Lerp(Value3D(x0, y1, z0, seed), Value3D(x1, y1, z0, seed), tx);
    const float x01 = Lerp(Value3D(x0, y0, z1, seed), Value3D(x1, y0, z1, seed), tx);
    const float x11 = Lerp(Value3D(x0, y1, z1, seed), Value3D(x1, y1, z1, seed), tx);
    const float y0v = Lerp(x00, x10, ty);
    const float y1v = Lerp(x01, x11, ty);
    return Lerp(y0v, y1v, tz);
}

inline float SignedValueNoise1D(float x, int32_t seed = 0)
{
    return ValueNoise1D(x, seed) * 2.0f - 1.0f;
}

inline float SignedValueNoise2D(Vec2 p, int32_t seed = 0)
{
    return ValueNoise2D(p, seed) * 2.0f - 1.0f;
}

inline float SignedValueNoise3D(Vec3 p, int32_t seed = 0)
{
    return ValueNoise3D(p, seed) * 2.0f - 1.0f;
}

inline float FractalValueNoise2D(Vec2 p, int32_t octaves, float lacunarity = 2.0f, float gain = 0.5f, int32_t seed = 0)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float norm = 0.0f;

    for (int32_t octave = 0; octave < octaves; ++octave)
    {
        sum += ValueNoise2D(p * frequency, seed + octave * 1013) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }

    return norm <= Epsilon ? 0.0f : sum / norm;
}

inline float FractalValueNoise3D(Vec3 p, int32_t octaves, float lacunarity = 2.0f, float gain = 0.5f, int32_t seed = 0)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float norm = 0.0f;

    for (int32_t octave = 0; octave < octaves; ++octave)
    {
        sum += ValueNoise3D(p * frequency, seed + octave * 1013) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }

    return norm <= Epsilon ? 0.0f : sum / norm;
}

inline float RidgedValueNoise2D(Vec2 p, int32_t octaves, float lacunarity = 2.0f, float gain = 0.5f, int32_t seed = 0)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float norm = 0.0f;

    for (int32_t octave = 0; octave < octaves; ++octave)
    {
        const float value = 1.0f - Abs(SignedValueNoise2D(p * frequency, seed + octave * 1013));
        sum += value * value * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }

    return norm <= Epsilon ? 0.0f : sum / norm;
}
}
