#include "TerrainSplatPaint.h"

#include <cmath>
#include <iostream>
#include <numeric>
#include <random>

namespace
{
int checks = 0;
int failures = 0;

void Check(const char* name, bool condition)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

terrainpaint::Weights Layer(std::uint32_t slot)
{
    terrainpaint::Weights weights{};
    weights[slot] = 255;
    return weights;
}

int Total(const terrainpaint::Weights& weights)
{
    return std::accumulate(weights.begin(), weights.end(), 0);
}
}

int main()
{
    using terrainpaint::Paint;
    using terrainpaint::Weights;
    using terrainpaint::StrokeCoverage;
    using terrainpaint::Stroke;
    using terrainpaint::Point;

    const Stroke dot{{0.0f, 0.0f}, {0.0f, 0.0f}, 8.0f, 1.0f, 1.0f};
    // All eight layers must support a complete texture change in a single stroke.
    for (std::uint32_t oldSlot = 0; oldSlot < 8; ++oldSlot)
        for (std::uint32_t newSlot = 0; newSlot < 8; ++newSlot)
            for (float smoothing : {0.0f, 0.5f, 1.0f})
                for (float distance : {0.0f, 1.0f, 2.0f})
                    Check("Full coverage removes all previous layers in the interior",
                        Paint(Layer(oldSlot), newSlot, StrokeCoverage({distance, 0.0f}, dot, smoothing, 1.0f)) == Layer(newSlot));

    const Weights mixed{20, 30, 35, 10, 15, 55, 60, 30};
    const float edge = StrokeCoverage({5.0f, 0.0f}, dot, 1.0f, 1.0f);
    Check("Maximum smoothing provides a broad half-covered transition", std::abs(edge - 0.5f) < 0.00001f);
    Check("Smoothing eases into the original texture",
        StrokeCoverage({7.2f, 0.0f}, dot, 1.0f, 1.0f) > 0.0f &&
        StrokeCoverage({7.2f, 0.0f}, dot, 1.0f, 1.0f) < 0.025f);
    const auto half = Paint(Layer(0), 5, edge);
    Check("Half coverage retains half the previous texture", half[0] == 127 && half[5] == 128 && Total(half) == 255);
    Check("Empty weights blend from the shader default layer", Paint(Weights{}, 5, edge) == half);
    Check("Soft edges preserve already solid paint", Paint(Layer(5), 5, edge) == Layer(5));

    // Coverage scales the entire brush independently of edge smoothing; repeated applications
    // must not build up opacity or depend on how many frames the user holds the mouse button.
    for (float smoothing : {0.0f, 0.001f, 0.5f, 1.0f})
        for (float coverage : {0.0f, 0.25f, 0.5f, 1.0f})
        {
            const float center = StrokeCoverage(dot.start, dot, smoothing, coverage);
            Check("Center always has the chosen overall coverage", center == coverage);
            const auto first = Paint(Layer(0), 5, center);
            auto repeated = first;
            for (int i = 0; i < 1000; ++i)
                repeated = Paint(repeated, 5, center);
            Check("Holding brush does not accumulate coverage", repeated == first);
            Check("Center weight matches overall coverage", first[5] == std::lround(255.0f * coverage));
            Check("Zero coverage leaves existing paint unchanged",
                Paint(mixed, 3, StrokeCoverage({4.0f, 0.0f}, dot, smoothing, 0.0f)) == mixed);
            for (float distance : {0.0f, 2.0f, 5.0f, 7.9f})
                Check("Overall coverage scales the interior and edge equally",
                    std::abs(StrokeCoverage({distance, 0.0f}, dot, smoothing, coverage) -
                        coverage * StrokeCoverage({distance, 0.0f}, dot, smoothing, 1.0f)) < 0.00001f);
            Check("Outside brush is untouched at every setting",
                StrokeCoverage({8.001f, 0.0f}, dot, smoothing, coverage) == 0.0f);
            Check("Exact outer boundary is untouched",
                StrokeCoverage({8.0f, 0.0f}, dot, smoothing, coverage) == 0.0f);
        }

    // Slider changes must be continuous, with no hidden switch at zero smoothing.
    float previousEdge = StrokeCoverage({7.9f, 0.0f}, dot, 0.0f, 1.0f);
    for (int step = 1; step <= 1000; ++step)
    {
        const float current = StrokeCoverage({7.9f, 0.0f}, dot, step / 1000.0f, 1.0f);
        Check("Increasing smoothing never sharpens the edge", current <= previousEdge + 0.000001f);
        Check("Smoothing slider has no discontinuous mode change", std::abs(current - previousEdge) < 0.025f);
        previousEdge = current;
    }
    Stroke largerDot = dot;
    largerDot.radius *= 2.0f;
    Check("Increasing diameter expands the painted area",
        StrokeCoverage({10.0f, 0.0f}, dot, 0.0f, 1.0f) == 0.0f &&
        StrokeCoverage({10.0f, 0.0f}, largerDot, 0.0f, 1.0f) == 1.0f);

    // Fast dragging fills the gaps between cursor samples while keeping straight contours.
    const Stroke horizontal{{0.0f, 0.0f}, {20.0f, 0.0f}, 2.9f, 1.0f, 1.0f};
    for (float x : {3.0f, 7.5f, 11.0f, 17.0f})
    {
        Check("Fast dragging fills the full line interior",
            Paint(Layer(0), 5, StrokeCoverage({x, 2.0f}, horizontal, 0.0f, 1.0f)) == Layer(5));
        Check("Crisp straight line has constant antialiasing",
            std::abs(StrokeCoverage({x, 2.775f}, horizontal, 0.0f, 1.0f) - 0.5f) < 0.00001f);
        Check("Minimal smoothing is confined to a narrow inner edge",
            StrokeCoverage({x, 2.64f}, horizontal, 0.0f, 1.0f) == 1.0f);
        Check("No smoothing setting paints outside the contour",
            StrokeCoverage({x, 2.91f}, horizontal, 0.0f, 1.0f) == 0.0f &&
            StrokeCoverage({x, 2.91f}, horizontal, 1.0f, 1.0f) == 0.0f);
    }
    const Stroke smallBrush{{0.0f, 0.0f}, {0.0f, 0.0f}, 0.5f, 10.0f, 10.0f};
    for (float smoothing : {0.0f, 0.5f, 1.0f})
        Check("Small brush keeps a solid center on a coarse mask",
            StrokeCoverage(smallBrush.start, smallBrush, smoothing, 1.0f) == 1.0f);

    const Stroke diagonal{{0.0f, 0.0f}, {20.0f, 10.0f}, 2.9f, 1.0f, 0.5f};
    for (float position : {0.2f, 0.4f, 0.6f, 0.8f})
    {
        const Point sample{20.0f * position - 2.775f / std::sqrt(5.0f),
            10.0f * position + 5.55f / std::sqrt(5.0f)};
        Check("Diagonal antialiasing stays constant on non-square texels",
            std::abs(StrokeCoverage(sample, diagonal, 0.0f, 1.0f) - 0.5f) < 0.00001f);
    }
    // Sparse and dense mouse events must paint the same capsule at every brush setting.
    for (float smoothing : {0.0f, 0.5f, 1.0f})
        for (float coverage : {0.25f, 1.0f})
            for (int y = -5; y <= 15; ++y)
                for (int x = -5; x <= 25; ++x)
                {
                    const Point sample{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
                    const auto sparse = Paint(Layer(0), 6, StrokeCoverage(sample, diagonal, smoothing, coverage));
                    auto dense = Layer(0);
                    for (int part = 0; part < 20; ++part)
                    {
                        const Stroke segment{{static_cast<float>(part), 0.5f * part},
                            {static_cast<float>(part + 1), 0.5f * (part + 1)}, diagonal.radius, diagonal.texelSizeX, diagonal.texelSizeY};
                        dense = Paint(dense, 6, StrokeCoverage(sample, segment, smoothing, coverage));
                    }
                    Check("Straight stroke does not depend on cursor sampling rate",
                        std::abs(static_cast<int>(sparse[6]) - dense[6]) <= 1);
                    Check("Stroke weights stay normalized", Total(sparse) == 255 && Total(dense) == 255);
                }

    // Old non-normalized data and all eight channels must stay stable after quantization.
    std::mt19937 random(20261010);
    for (int sample = 0; sample < 4096; ++sample)
    {
        Weights before{};
        for (auto& weight : before)
            weight = static_cast<std::uint8_t>(random() & 255u);
        const std::uint32_t slot = random() % 8u;
        const float coverage = static_cast<float>(1u + random() % 255u) / 255.0f;
        const auto after = Paint(before, slot, coverage);
        Check("Quantized weights sum to exactly 255", Total(after) == 255);
        Check("Repeat stamp is independent of application count", Paint(after, slot, coverage) == after);
        Check("Selected layer reaches requested coverage", after[slot] >= std::lround(coverage * 255.0f));
    }
    Check("Invalid layer is a no-op", Paint(mixed, 8, 1.0f) == mixed);
    Stroke emptyBrush = dot;
    emptyBrush.radius = 0.0f;
    Check("Zero diameter is a no-op", StrokeCoverage(dot.start, emptyBrush, 0.5f, 1.0f) == 0.0f);
    Check("Coverage outside slider range is clamped",
        StrokeCoverage(dot.start, dot, 0.5f, -1.0f) == 0.0f &&
        StrokeCoverage(dot.start, dot, 0.5f, 2.0f) == 1.0f);
    std::cout << "Terrain splat paint: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
