#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Static mob definitions loaded from mob_types.conf. Read-only after load;
// shared by spawn, AI and combat. Lookup by mob type id.
namespace gs::game {

struct MobTypeDefinition {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t model_id = 0;
    std::uint32_t hp_max = 1;
    std::uint32_t damage = 0;
    float speed = 0.0f;
    float wander_speed = 1.5f;
    float wander_idle_min = 3.0f;
    float wander_idle_max = 8.0f;
    float defense = 0.0f;
    float attack_range = 2.0f;
    float attack_cooldown = 1.5f;
    float respawn_time_sec = 30.0f;
};

class MobPrototypeRegistry {
public:
    bool LoadFromFile(const std::string& path);

    const MobTypeDefinition* Find(std::uint32_t mob_type_id) const;
    bool Empty() const noexcept
    {
        return types_.empty();
    }
    std::size_t Size() const noexcept
    {
        return types_.size();
    }

private:
    std::unordered_map<std::uint32_t, MobTypeDefinition> types_;
};

} // namespace gs::game
