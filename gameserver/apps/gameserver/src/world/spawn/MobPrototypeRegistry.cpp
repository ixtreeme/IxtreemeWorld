#include "MobPrototypeRegistry.h"

#include <cmath>
#include <exception>
#include <fstream>

#include "common/Logging.h"

namespace gs::game {
namespace {

std::string TrimCopy(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string StripComment(std::string value)
{
    const auto comment = value.find('#');
    if (comment != std::string::npos) {
        value.erase(comment);
    }
    return TrimCopy(std::move(value));
}

bool ParseKeyValue(const std::string& token, std::string& key, std::string& value)
{
    const auto equals = token.find('=');
    if (equals == std::string::npos) {
        return false;
    }
    key = TrimCopy(token.substr(0, equals));
    value = TrimCopy(token.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return !key.empty();
}

} // namespace

bool MobPrototypeRegistry::LoadFromFile(const std::string& path)
{
    types_.clear();

    std::ifstream file(path);
    if (!file) {
        LOG_WARN("Mob type config '{}' not found; no mobs will spawn", path);
        return false;
    }

    MobTypeDefinition current;
    bool in_mob = false;
    auto commit = [&]() {
        if (!in_mob) {
            return;
        }
        if (current.id == 0 || current.name.empty()) {
            LOG_WARN("Skipping invalid mob type: id={} name='{}'", current.id, current.name);
        } else if (types_.contains(current.id)) {
            LOG_WARN("Skipping duplicate mob type id={} name='{}'", current.id, current.name);
        } else {
            if (!std::isfinite(current.wander_speed) || current.wander_speed < 0.0f) {
                current.wander_speed = 1.5f;
            }
            if (current.hp_max == 0) {
                current.hp_max = 50;
            }
            if (!std::isfinite(current.defense) || current.defense < 0.0f) {
                current.defense = 0.0f;
            }
            if (!std::isfinite(current.attack_range) || current.attack_range <= 0.0f) {
                current.attack_range = 2.0f;
            }
            if (!std::isfinite(current.attack_cooldown) || current.attack_cooldown < 0.0f) {
                current.attack_cooldown = 1.5f;
            }
            if (!std::isfinite(current.respawn_time_sec) || current.respawn_time_sec < 0.0f) {
                current.respawn_time_sec = 30.0f;
            }
            if (!std::isfinite(current.wander_idle_min) || current.wander_idle_min < 0.0f) {
                current.wander_idle_min = 3.0f;
            }
            if (!std::isfinite(current.wander_idle_max) || current.wander_idle_max < current.wander_idle_min) {
                current.wander_idle_max = std::max(current.wander_idle_min, 8.0f);
            }
            types_.emplace(current.id, current);
            LOG_INFO("Mob type loaded: id={} name='{}' model_id={} hp_max={} damage={} defense={} attack_range={} cooldown={} respawn={} speed={} wander_speed={} idle=[{}, {}]",
                     current.id,
                     current.name,
                     current.model_id,
                     current.hp_max,
                     current.damage,
                     current.defense,
                     current.attack_range,
                     current.attack_cooldown,
                     current.respawn_time_sec,
                     current.speed,
                     current.wander_speed,
                     current.wander_idle_min,
                     current.wander_idle_max);
        }
        current = {};
        in_mob = false;
    };

    std::string line;
    while (std::getline(file, line)) {
        line = StripComment(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line == "[mob]") {
            commit();
            current = {};
            in_mob = true;
            continue;
        }
        if (!in_mob) {
            LOG_WARN("Ignoring mob type config line outside [mob]: {}", line);
            continue;
        }

        std::string key;
        std::string value;
        if (!ParseKeyValue(line, key, value)) {
            LOG_WARN("Ignoring invalid mob type config line: {}", line);
            continue;
        }
        try {
            if (key == "id") {
                current.id = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "name") {
                current.name = value;
            } else if (key == "model_id") {
                current.model_id = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "hp_max") {
                current.hp_max = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "damage") {
                current.damage = static_cast<std::uint32_t>(std::stoul(value));
            } else if (key == "defense") {
                current.defense = std::stof(value);
            } else if (key == "attack_range") {
                current.attack_range = std::stof(value);
            } else if (key == "attack_cooldown") {
                current.attack_cooldown = std::stof(value);
            } else if (key == "respawn_time_sec") {
                current.respawn_time_sec = std::stof(value);
            } else if (key == "speed") {
                current.speed = std::stof(value);
            } else if (key == "wander_speed") {
                current.wander_speed = std::stof(value);
            } else if (key == "wander_idle_min") {
                current.wander_idle_min = std::stof(value);
            } else if (key == "wander_idle_max") {
                current.wander_idle_max = std::stof(value);
            }
        } catch (const std::exception& error) {
            LOG_WARN("Invalid mob type value '{}={}' ({})", key, value, error.what());
        }
    }
    commit();
    LOG_INFO("Mob type registry ready: count={}", types_.size());
    return true;
}

const MobTypeDefinition* MobPrototypeRegistry::Find(std::uint32_t mob_type_id) const
{
    const auto it = types_.find(mob_type_id);
    return it != types_.end() ? &it->second : nullptr;
}

} // namespace gs::game
