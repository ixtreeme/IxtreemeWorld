#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Static mob description. MobTypeRef points into MobPrototypeRegistry;
// MobSpawnRef points back to the spawn point for respawn scheduling;
// MobProfile carries the display/replication data for the mob instance.
// All authoritative on the flecs entity.
namespace gs::game {

struct MobTypeRef {
    std::uint32_t id = 0;
};

struct MobSpawnRef {
    std::size_t spawn_point_index = 0;
};

struct MobProfile {
    std::uint32_t model_id = 0;
    std::uint32_t level = 1;
    std::string name;
};

} // namespace gs::game
