#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Spawn point data loaded from mob_spawns.conf. Plain data; the SpawnSystem
// turns these into flecs entities, RespawnSystem schedules their return.
namespace gs::game {

struct MobSpawnPoint {
    std::uint32_t mob_type_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t count = 0;
    float radius = 0.0f;
};

class SpawnLoader {
public:
    static std::vector<MobSpawnPoint> LoadFromFile(const std::string& path);
};

} // namespace gs::game
