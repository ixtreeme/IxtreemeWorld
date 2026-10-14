#include "SpawnLoader.h"

#include <cmath>
#include <exception>
#include <fstream>
#include <sstream>

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

std::string StripSpawnComment(std::string value)
{
    const auto comment = value.find('#');
    if (comment != std::string::npos) {
        value.erase(comment);
    }
    return TrimCopy(std::move(value));
}

bool ParseSpawnKeyValue(const std::string& token, std::string& key, std::string& value)
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

std::vector<MobSpawnPoint> SpawnLoader::LoadFromFile(const std::string& path)
{
    std::vector<MobSpawnPoint> points;

    std::ifstream file(path);
    if (!file) {
        LOG_WARN("Mob spawn config '{}' not found; no mobs will spawn", path);
        return points;
    }

    std::string line;
    std::uint32_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        line = StripSpawnComment(std::move(line));
        if (line.empty()) {
            continue;
        }

        MobSpawnPoint spawn;
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            std::string key;
            std::string value;
            if (!ParseSpawnKeyValue(token, key, value)) {
                continue;
            }
            try {
                if (key == "mob_type_id") {
                    spawn.mob_type_id = static_cast<std::uint32_t>(std::stoul(value));
                } else if (key == "x") {
                    spawn.x = std::stof(value);
                } else if (key == "y") {
                    spawn.y = std::stof(value);
                } else if (key == "count") {
                    spawn.count = static_cast<std::uint32_t>(std::stoul(value));
                } else if (key == "radius") {
                    spawn.radius = std::max(0.0f, std::stof(value));
                }
            } catch (const std::exception& error) {
                LOG_WARN("Invalid mob spawn value at {}:{} '{}={}' ({})",
                         path,
                         line_number,
                         key,
                         value,
                         error.what());
            }
        }

        if (spawn.mob_type_id == 0 || spawn.count == 0 || !std::isfinite(spawn.x) ||
            !std::isfinite(spawn.y) || !std::isfinite(spawn.radius)) {
            LOG_WARN("Skipping invalid mob spawn at {}:{} '{}'", path, line_number, line);
            continue;
        }
        points.push_back(spawn);
    }
    LOG_INFO("Mob spawn config loaded: points={}", points.size());
    return points;
}

} // namespace gs::game
