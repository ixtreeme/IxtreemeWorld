#pragma once

#include "Scalar.h"

#include <math.h>
#include <cmath>

namespace ixtreeme::math::ease
{
inline float Linear(float t)
{
    return t;
}

inline float InSine(float t)
{
    return 1.0f - Cos((t * Pi) * 0.5f);
}

inline float OutSine(float t)
{
    return Sin((t * Pi) * 0.5f);
}

inline float InOutSine(float t)
{
    return -(Cos(Pi * t) - 1.0f) * 0.5f;
}

inline float InQuad(float t) { return t * t; }
inline float OutQuad(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
inline float InOutQuad(float t) { return t < 0.5f ? 2.0f * t * t : 1.0f - Square(-2.0f * t + 2.0f) * 0.5f; }

inline float InCubic(float t) { return t * t * t; }
inline float OutCubic(float t) { return 1.0f - Cube(1.0f - t); }
inline float InOutCubic(float t) { return t < 0.5f ? 4.0f * t * t * t : 1.0f - Cube(-2.0f * t + 2.0f) * 0.5f; }

inline float InQuart(float t) { return t * t * t * t; }
inline float OutQuart(float t) { return 1.0f - Square(Square(1.0f - t)); }
inline float InOutQuart(float t) { return t < 0.5f ? 8.0f * Square(Square(t)) : 1.0f - Square(Square(-2.0f * t + 2.0f)) * 0.5f; }

inline float InQuint(float t) { return t * t * t * t * t; }
inline float OutQuint(float t) { return 1.0f - Pow(1.0f - t, 5.0f); }
inline float InOutQuint(float t) { return t < 0.5f ? 16.0f * Pow(t, 5.0f) : 1.0f - Pow(-2.0f * t + 2.0f, 5.0f) * 0.5f; }

inline float InExpo(float t)
{
    return t <= 0.0f ? 0.0f : Pow(2.0f, 10.0f * t - 10.0f);
}

inline float OutExpo(float t)
{
    return t >= 1.0f ? 1.0f : 1.0f - Pow(2.0f, -10.0f * t);
}

inline float InOutExpo(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t < 0.5f ? Pow(2.0f, 20.0f * t - 10.0f) * 0.5f : (2.0f - Pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

inline float InCirc(float t)
{
    return 1.0f - Sqrt(1.0f - t * t);
}

inline float OutCirc(float t)
{
    return Sqrt(1.0f - Square(t - 1.0f));
}

inline float InOutCirc(float t)
{
    return t < 0.5f
        ? (1.0f - Sqrt(1.0f - Square(2.0f * t))) * 0.5f
        : (Sqrt(1.0f - Square(-2.0f * t + 2.0f)) + 1.0f) * 0.5f;
}

inline float InBack(float t, float overshoot = 1.70158f)
{
    return (overshoot + 1.0f) * t * t * t - overshoot * t * t;
}

inline float OutBack(float t, float overshoot = 1.70158f)
{
    const float x = t - 1.0f;
    return 1.0f + (overshoot + 1.0f) * x * x * x + overshoot * x * x;
}

inline float InOutBack(float t, float overshoot = 1.70158f)
{
    const float c = overshoot * 1.525f;
    if (t < 0.5f)
    {
        const float x = 2.0f * t;
        return (x * x * ((c + 1.0f) * x - c)) * 0.5f;
    }
    const float x = 2.0f * t - 2.0f;
    return (x * x * ((c + 1.0f) * x + c) + 2.0f) * 0.5f;
}

inline float OutBounce(float t)
{
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;

    if (t < 1.0f / d1)
        return n1 * t * t;
    if (t < 2.0f / d1)
    {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1)
    {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

inline float InBounce(float t)
{
    return 1.0f - OutBounce(1.0f - t);
}

inline float InOutBounce(float t)
{
    return t < 0.5f ? (1.0f - OutBounce(1.0f - 2.0f * t)) * 0.5f : (1.0f + OutBounce(2.0f * t - 1.0f)) * 0.5f;
}

inline float InElastic(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    constexpr float c = TwoPi / 3.0f;
    return -Pow(2.0f, 10.0f * t - 10.0f) * Sin((t * 10.0f - 10.75f) * c);
}

inline float OutElastic(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    constexpr float c = TwoPi / 3.0f;
    return Pow(2.0f, -10.0f * t) * Sin((t * 10.0f - 0.75f) * c) + 1.0f;
}

inline float InOutElastic(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    constexpr float c = TwoPi / 4.5f;
    if (t < 0.5f)
        return -(Pow(2.0f, 20.0f * t - 10.0f) * Sin((20.0f * t - 11.125f) * c)) * 0.5f;
    return Pow(2.0f, -20.0f * t + 10.0f) * Sin((20.0f * t - 11.125f) * c) * 0.5f + 1.0f;
}
}
