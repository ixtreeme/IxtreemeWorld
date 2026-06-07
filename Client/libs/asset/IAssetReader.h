#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace client::asset {

class IAssetReader {
public:
    virtual ~IAssetReader() = default;

    virtual std::optional<std::vector<std::uint8_t>> ReadAll(std::string_view path) const = 0;
    virtual std::optional<std::filesystem::path> RootPath() const { return std::nullopt; }

    std::optional<std::string> ReadText(std::string_view path) const
    {
        auto bytes = ReadAll(path);
        if (!bytes) {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }
};

} // namespace client::asset
