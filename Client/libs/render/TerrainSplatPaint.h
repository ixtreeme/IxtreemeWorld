#pragma once

#include <array>
#include <cstdint>

namespace terrainpaint
{
using Weights = std::array<std::uint8_t, 8>;

struct Point
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Stroke
{
    Point start;
    Point end;
    float radius = 1.0f;
    float texelSizeX = 1.0f;
    float texelSizeY = 1.0f;
};

// Continuous capsule between cursor samples. Smoothing varies the inner edge ramp from minimal
// antialiasing to a broad feather; overall coverage scales both the interior and the edge (0..1).
float StrokeCoverage(Point sample, const Stroke& stroke, float edgeSmoothing, float overallCoverage);

// Raises the selected layer to the brush coverage, proportionally retaining the other layers.
// Repeating the same stamp is idempotent; every changed texel has exactly 255 total weight.
Weights Paint(const Weights& previous, std::uint32_t slot, float coverage);
}
