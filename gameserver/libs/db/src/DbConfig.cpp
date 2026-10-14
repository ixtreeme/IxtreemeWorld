#include "db/DbConfig.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace gs::db {
namespace {

template <typename T>
T GetOr(const nlohmann::json& json, const char* key, T fallback)
{
    if (!json.contains(key)) {
        return fallback;
    }
    try {
        return json.at(key).get<T>();
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("invalid field '") + key + "': " + e.what());
    }
}

std::string GetRequiredString(const nlohmann::json& json, const char* key)
{
    if (!json.contains(key)) {
        throw std::runtime_error(std::string("missing field '") + key + "'");
    }
    if (!json.at(key).is_string()) {
        throw std::runtime_error(std::string("invalid field '") + key + "': expected string");
    }
    auto value = json.at(key).get<std::string>();
    if (value.empty()) {
        throw std::runtime_error(std::string("invalid field '") + key + "': must not be empty");
    }
    return value;
}

} // namespace

DbConfig LoadDbConfig(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("failed to open database config: " + path.string());
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("failed to parse database config: " + std::string(e.what()));
    }

    DbConfig config;
    config.host = GetOr<std::string>(json, "host", config.host);
    config.port = GetOr<std::uint16_t>(json, "port", config.port);
    config.user = GetRequiredString(json, "user");
    config.password = GetRequiredString(json, "password");
    config.database = GetRequiredString(json, "database");
    config.pool_size = GetOr<std::uint32_t>(json, "pool_size", config.pool_size);
    config.thread_pool_size =
        GetOr<std::uint32_t>(json, "thread_pool_size", config.thread_pool_size);
    config.connect_timeout_ms =
        GetOr<std::uint32_t>(json, "connect_timeout_ms", config.connect_timeout_ms);

    if (config.pool_size == 0) {
        throw std::runtime_error("invalid field 'pool_size': must be greater than zero");
    }
    if (config.thread_pool_size == 0) {
        throw std::runtime_error("invalid field 'thread_pool_size': must be greater than zero");
    }

    return config;
}

} // namespace gs::db
