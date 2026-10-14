#pragma once

#include <cmath>
#include <cstdint>
#include <random>

#include "../WorldConstants.h"
#include "../components/AiComponents.h"

// Deterministic spawn/AI randomness helpers (moved verbatim from SimWorld).
namespace gs::game {

inline std::uint32_t MakeMobSeed(std::uint32_t net_id, std::uint32_t mob_type_id)
{
    std::uint32_t seed = 0x9e3779b9u;
    seed ^= net_id + 0x85ebca6bu + (seed << 6) + (seed >> 2);
    seed ^= mob_type_id + 0xc2b2ae35u + (seed << 6) + (seed >> 2);
    return seed;
}

inline float RandomRange(std::mt19937& rng, float min_value, float max_value)
{
    if (!std::isfinite(min_value) || !std::isfinite(max_value)) {
        return 0.0f;
    }
    if (max_value < min_value) {
        std::swap(min_value, max_value);
    }
    std::uniform_real_distribution<float> dist(min_value, max_value);
    return dist(rng);
}

inline Vec2 RandomPointInCircle(std::mt19937& rng, Vec2 center, float radius)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float angle = unit(rng) * kTwoPi;
    const float distance = std::max(0.0f, radius) * std::sqrt(unit(rng));
    return Vec2{center.x + std::cos(angle) * distance,
                center.y + std::sin(angle) * distance};
}

} // namespace gs::game
