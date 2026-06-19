#include "LODSystem.h"

#include <algorithm>
#include <cmath>
#include <functional>

std::uint64_t HashLodConfig(const LodConfig& config)
{
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(std::clamp(config.levelCount, 1u, LodConfig::MaxLevels));
    mix(static_cast<std::uint32_t>(std::max(0.0f, config.hysteresisMeters) * 100.0f));
    for (std::uint32_t i = 0; i < LodConfig::MaxLevels; ++i)
    {
        mix(static_cast<std::uint32_t>(std::clamp(config.targetRatios[i], 0.001f, 1.0f) * 100000.0f));
        mix(static_cast<std::uint32_t>(std::max(0.0f, config.distances[i]) * 100.0f));
    }
    return hash == 0 ? 1 : hash;
}

float DistanceToAabb(const WorldVec3& point, const SpatialIndex::Aabb& bounds)
{
    const float dx = std::max({bounds.min.x - point.x, 0.0f, point.x - bounds.max.x});
    const float dy = std::max({bounds.min.y - point.y, 0.0f, point.y - bounds.max.y});
    const float dz = std::max({bounds.min.z - point.z, 0.0f, point.z - bounds.max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::uint32_t SelectLodLevel(const LodConfig& config, float distanceMeters, std::uint32_t previousLevel)
{
    const std::uint32_t levelCount = std::clamp(config.levelCount, 1u, LodConfig::MaxLevels);
    std::uint32_t selected = 0;
    for (std::uint32_t level = 1; level < levelCount; ++level)
    {
        if (distanceMeters >= config.distances[level])
            selected = level;
    }

    const float hysteresis = std::max(0.0f, config.hysteresisMeters);
    if (previousLevel < levelCount && previousLevel != selected && hysteresis > 0.0f)
    {
        if (previousLevel < selected && distanceMeters < config.distances[selected] + hysteresis)
            return previousLevel;
        if (previousLevel > selected && distanceMeters > config.distances[previousLevel] - hysteresis)
            return previousLevel;
    }
    return selected;
}

std::size_t StaticMeshLodBatchKeyHash::operator()(const StaticMeshLodBatchKey& key) const
{
    std::size_t hash = std::hash<StaticMeshRenderer*>{}(key.renderer);
    hash ^= std::hash<std::uint64_t>{}(key.configHash) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<std::uint32_t>{}(key.lodLevel) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}
