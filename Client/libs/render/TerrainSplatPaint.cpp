#include "TerrainSplatPaint.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace terrainpaint
{
float StrokeCoverage(Point sample, const Stroke& stroke, float edgeSmoothing, float overallCoverage)
{
    if (!std::isfinite(stroke.radius) || stroke.radius <= 0.0f ||
        !std::isfinite(edgeSmoothing) || !std::isfinite(overallCoverage) || overallCoverage <= 0.0f)
        return 0.0f;
    const float vx = stroke.end.x - stroke.start.x;
    const float vy = stroke.end.y - stroke.start.y;
    const float lengthSquared = vx * vx + vy * vy;
    const float projection = lengthSquared > 0.000001f
        ? std::clamp(((sample.x - stroke.start.x) * vx + (sample.y - stroke.start.y) * vy) / lengthSquared, 0.0f, 1.0f)
        : 0.0f;
    const float dx = sample.x - (stroke.start.x + projection * vx);
    const float dy = sample.y - (stroke.start.y + projection * vy);
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(distance) || distance >= stroke.radius)
        return 0.0f;

    // Keep the footprint constant along a stroke: max-combining overlapping capsules must be the
    // same as sweeping one longer capsule. A normal-dependent width makes joins grow little bumps.
    // The narrowest ramp stays inside the contour and is capped for small brushes. Smoothing
    // widens it continuously, without a mode change or a jump at the first slider increment.
    const float footprint = std::min(0.25f * std::max(stroke.texelSizeX, stroke.texelSizeY), 0.1f * stroke.radius);
    const float edgeWidth = std::lerp(footprint, 0.75f * stroke.radius, std::clamp(edgeSmoothing, 0.0f, 1.0f));
    const float t = std::clamp((stroke.radius - distance) / std::max(edgeWidth, 0.000001f), 0.0f, 1.0f);
    const float feather = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    return feather * std::clamp(overallCoverage, 0.0f, 1.0f);
}

Weights Paint(const Weights& previous, std::uint32_t slot, float coverage)
{
    if (slot >= previous.size() || !std::isfinite(coverage) || coverage <= 0.0f)
        return previous;
    if (coverage >= 1.0f)
    {
        Weights result{};
        result[slot] = 255;
        return result;
    }

    Weights base = previous;
    int total = std::accumulate(base.begin(), base.end(), 0);
    if (total == 0)
    {
        // An unpainted splat uses layer zero in the terrain shader.
        base[0] = 255;
        total = 255;
    }
    const int current = static_cast<int>(std::lround(255.0f * base[slot] / total));
    const int target = std::max(current,
        static_cast<int>(std::lround(255.0f * std::clamp(coverage, 0.0f, 1.0f))));
    Weights result{};
    result[slot] = static_cast<std::uint8_t>(target);
    const int otherTotal = total - base[slot];
    if (target == 255 || otherTotal == 0)
    {
        result[slot] = 255;
        return result;
    }

    // Distribute the remaining byte budget proportionally, then assign rounding remainders.
    // This preserves exact normalization and does not accumulate rounding drift on later stamps.
    const int budget = 255 - target;
    std::array<int, 8> remainders{};
    int remaining = budget;
    for (std::uint32_t i = 0; i < result.size(); ++i)
    {
        if (i == slot)
            continue;
        const int numerator = budget * base[i];
        result[i] = static_cast<std::uint8_t>(numerator / otherTotal);
        remainders[i] = numerator % otherTotal;
        remaining -= result[i];
    }
    while (remaining > 0)
    {
        const auto largest = std::max_element(remainders.begin(), remainders.end());
        ++result[static_cast<std::size_t>(largest - remainders.begin())];
        *largest = 0;
        --remaining;
    }
    return result;
}
}
