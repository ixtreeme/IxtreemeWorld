#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace gs::common {

class Config {
public:
    bool Load(const std::filesystem::path& path);

    [[nodiscard]] std::optional<std::string> GetString(const std::string& key) const;
    [[nodiscard]] std::optional<int> GetInt(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace gs::common
