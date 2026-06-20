#pragma once

#include <algorithm>
#include <math.h>
#include <cmath>
#include <limits>
#include <type_traits>

namespace ixtreeme::math
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float HalfPi = Pi * 0.5f;
constexpr float TwoPi = Pi * 2.0f;
constexpr float DegToRad = Pi / 180.0f;
constexpr float RadToDeg = 180.0f / Pi;
constexpr float Epsilon = 0.000001f;

template <typename T>
constexpr T Min(T a, T b)
{
    return a < b ? a : b;
}

template <typename T>
constexpr T Max(T a, T b)
{
    return a > b ? a : b;
}

template <typename T>
constexpr T Clamp(T value, T minValue, T maxValue)
{
    return Max(minValue, Min(value, maxValue));
}

constexpr float Saturate(float value)
{
    return Clamp(value, 0.0f, 1.0f);
}

template <typename T>
constexpr T Abs(T value)
{
    return value < T{} ? -value : value;
}

template <typename T>
constexpr int Sign(T value)
{
    return (T{} < value) - (value < T{});
}

template <typename T>
constexpr T Square(T value)
{
    return value * value;
}

template <typename T>
constexpr T Cube(T value)
{
    return value * value * value;
}

inline bool IsFinite(float value)
{
    return std::isfinite(value);
}

inline bool IsNan(float value)
{
    return std::isnan(value);
}

inline bool NearlyEqual(float a, float b, float epsilon = Epsilon)
{
    return Abs(a - b) <= epsilon;
}

inline float Sqrt(float value)
{
    return std::sqrt(value);
}

inline float Sin(float radians)
{
    return std::sin(radians);
}

inline float Cos(float radians)
{
    return std::cos(radians);
}

inline float Tan(float radians)
{
    return std::tan(radians);
}

inline float Asin(float value)
{
    return std::asin(value);
}

inline float Acos(float value)
{
    return std::acos(value);
}

inline float Atan2(float y, float x)
{
    return std::atan2(y, x);
}

inline float Pow(float base, float exponent)
{
    return std::pow(base, exponent);
}

inline float Exp(float value)
{
    return std::exp(value);
}

inline float Floor(float value)
{
    return std::floor(value);
}

inline float Ceil(float value)
{
    return std::ceil(value);
}

inline float Round(float value)
{
    return std::round(value);
}

inline float CopySign(float magnitude, float sign)
{
    return std::copysign(magnitude, sign);
}

inline float Cbrt(float value)
{
    return std::cbrt(value);
}

constexpr float DegreesToRadians(float degrees)
{
    return degrees * DegToRad;
}

constexpr float RadiansToDegrees(float radians)
{
    return radians * RadToDeg;
}

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline float InverseLerp(float a, float b, float value)
{
    const float denom = b - a;
    if (Abs(denom) <= Epsilon)
        return 0.0f;
    return (value - a) / denom;
}

inline float Remap(float inMin, float inMax, float outMin, float outMax, float value)
{
    return Lerp(outMin, outMax, InverseLerp(inMin, inMax, value));
}

inline float SmoothStep(float edge0, float edge1, float value)
{
    const float t = Saturate(InverseLerp(edge0, edge1, value));
    return t * t * (3.0f - 2.0f * t);
}

inline float SmootherStep(float edge0, float edge1, float value)
{
    const float t = Saturate(InverseLerp(edge0, edge1, value));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float MoveTowards(float current, float target, float maxDelta)
{
    const float delta = target - current;
    if (Abs(delta) <= maxDelta)
        return target;
    return current + static_cast<float>(Sign(delta)) * maxDelta;
}

inline float Repeat(float value, float length)
{
    if (Abs(length) <= Epsilon)
        return 0.0f;
    return value - Floor(value / length) * length;
}

inline float PingPong(float value, float length)
{
    const float t = Repeat(value, length * 2.0f);
    return length - Abs(t - length);
}

inline float Wrap(float value, float minValue, float maxValue)
{
    const float length = maxValue - minValue;
    if (Abs(length) <= Epsilon)
        return minValue;
    return minValue + Repeat(value - minValue, length);
}

inline float Snap(float value, float step)
{
    if (Abs(step) <= Epsilon)
        return value;
    return Round(value / step) * step;
}

inline float DeltaAngleDegrees(float current, float target)
{
    float delta = Repeat(target - current, 360.0f);
    if (delta > 180.0f)
        delta -= 360.0f;
    return delta;
}

inline float UnwrapDegreesNear(float value, float reference)
{
    return reference + DeltaAngleDegrees(reference, value);
}
}
