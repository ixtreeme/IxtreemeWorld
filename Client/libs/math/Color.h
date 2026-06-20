#pragma once

#include "Scalar.h"

#include <math.h>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ixtreeme::math
{
struct Color
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    constexpr float& operator[](std::size_t index)
    {
        return index == 0 ? r : (index == 1 ? g : (index == 2 ? b : a));
    }

    constexpr const float& operator[](std::size_t index) const
    {
        return index == 0 ? r : (index == 1 ? g : (index == 2 ? b : a));
    }
};

struct Color32
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

constexpr Color ColorBlack(float alpha = 1.0f) { return {0.0f, 0.0f, 0.0f, alpha}; }
constexpr Color ColorWhite(float alpha = 1.0f) { return {1.0f, 1.0f, 1.0f, alpha}; }
constexpr Color ColorRed(float alpha = 1.0f) { return {1.0f, 0.0f, 0.0f, alpha}; }
constexpr Color ColorGreen(float alpha = 1.0f) { return {0.0f, 1.0f, 0.0f, alpha}; }
constexpr Color ColorBlue(float alpha = 1.0f) { return {0.0f, 0.0f, 1.0f, alpha}; }
constexpr Color ColorTransparent() { return {0.0f, 0.0f, 0.0f, 0.0f}; }

constexpr Color operator+(Color a, Color b) { return {a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a}; }
constexpr Color operator-(Color a, Color b) { return {a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a}; }
constexpr Color operator*(Color c, float s) { return {c.r * s, c.g * s, c.b * s, c.a * s}; }
constexpr Color operator*(float s, Color c) { return c * s; }
constexpr Color operator*(Color a, Color b) { return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a}; }
constexpr Color operator/(Color c, float s) { return {c.r / s, c.g / s, c.b / s, c.a / s}; }

inline Color ClampColor(Color color)
{
    return {
        Saturate(color.r),
        Saturate(color.g),
        Saturate(color.b),
        Saturate(color.a)};
}

inline Color Lerp(Color a, Color b, float t)
{
    return {
        Lerp(a.r, b.r, t),
        Lerp(a.g, b.g, t),
        Lerp(a.b, b.b, t),
        Lerp(a.a, b.a, t)};
}

inline Color LerpClamped(Color a, Color b, float t)
{
    return Lerp(a, b, Saturate(t));
}

inline float SrgbToLinearChannel(float value)
{
    value = Saturate(value);
    if (value <= 0.04045f)
        return value / 12.92f;
    return Pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSrgbChannel(float value)
{
    value = Saturate(value);
    if (value <= 0.0031308f)
        return value * 12.92f;
    return 1.055f * Pow(value, 1.0f / 2.4f) - 0.055f;
}

inline Color SrgbToLinear(Color color)
{
    return {
        SrgbToLinearChannel(color.r),
        SrgbToLinearChannel(color.g),
        SrgbToLinearChannel(color.b),
        color.a};
}

inline Color LinearToSrgb(Color color)
{
    return {
        LinearToSrgbChannel(color.r),
        LinearToSrgbChannel(color.g),
        LinearToSrgbChannel(color.b),
        color.a};
}

inline Color FromColor32(Color32 color, bool srgb = true)
{
    Color result{
        static_cast<float>(color.r) / 255.0f,
        static_cast<float>(color.g) / 255.0f,
        static_cast<float>(color.b) / 255.0f,
        static_cast<float>(color.a) / 255.0f};
    return srgb ? SrgbToLinear(result) : result;
}

inline uint8_t ToByte(float value)
{
    return static_cast<uint8_t>(Clamp(value, 0.0f, 255.0f));
}

inline Color32 ToColor32(Color color, bool srgb = true)
{
    const Color encoded = ClampColor(srgb ? LinearToSrgb(color) : color);
    return {
        ToByte(encoded.r * 255.0f + 0.5f),
        ToByte(encoded.g * 255.0f + 0.5f),
        ToByte(encoded.b * 255.0f + 0.5f),
        ToByte(encoded.a * 255.0f + 0.5f)};
}

inline float Luminance(Color linearColor)
{
    return linearColor.r * 0.2126f + linearColor.g * 0.7152f + linearColor.b * 0.0722f;
}

inline Color PremultiplyAlpha(Color color)
{
    return {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
}

inline Color UnpremultiplyAlpha(Color color)
{
    if (color.a <= Epsilon)
        return ColorTransparent();
    return {color.r / color.a, color.g / color.a, color.b / color.a, color.a};
}

inline Color HsvToRgb(float hue, float saturation, float value, float alpha = 1.0f)
{
    hue = Repeat(hue, 1.0f);
    saturation = Saturate(saturation);
    value = Saturate(value);

    const float h = hue * 6.0f;
    const int sector = static_cast<int>(Floor(h));
    const float f = h - static_cast<float>(sector);
    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - saturation * f);
    const float t = value * (1.0f - saturation * (1.0f - f));

    switch (sector % 6)
    {
    case 0:
        return {value, t, p, alpha};
    case 1:
        return {q, value, p, alpha};
    case 2:
        return {p, value, t, alpha};
    case 3:
        return {p, q, value, alpha};
    case 4:
        return {t, p, value, alpha};
    default:
        return {value, p, q, alpha};
    }
}

inline void RgbToHsv(Color color, float& hue, float& saturation, float& value)
{
    const float maxValue = Max(color.r, Max(color.g, color.b));
    const float minValue = Min(color.r, Min(color.g, color.b));
    const float delta = maxValue - minValue;

    value = maxValue;
    saturation = maxValue <= Epsilon ? 0.0f : delta / maxValue;

    if (delta <= Epsilon)
    {
        hue = 0.0f;
        return;
    }

    if (NearlyEqual(maxValue, color.r))
        hue = (color.g - color.b) / delta + (color.g < color.b ? 6.0f : 0.0f);
    else if (NearlyEqual(maxValue, color.g))
        hue = (color.b - color.r) / delta + 2.0f;
    else
        hue = (color.r - color.g) / delta + 4.0f;

    hue /= 6.0f;
}

inline Color ContrastColor(Color linearColor, float alpha = 1.0f)
{
    return Luminance(linearColor) > 0.5f ? ColorBlack(alpha) : ColorWhite(alpha);
}
}
