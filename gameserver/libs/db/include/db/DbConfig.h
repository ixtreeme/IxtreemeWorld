#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace gs::db {

struct DbConfig {
    std::string host = "localhost";
    std::uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database;
    std::uint32_t pool_size = 4;
    std::uint32_t thread_pool_size = 4;
    std::uint32_t connect_timeout_ms = 5000;
};

DbConfig LoadDbConfig(const std::filesystem::path& path);

} // namespace gs::db
