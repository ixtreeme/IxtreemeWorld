#pragma once

#include "asset/IAssetReader.h"

#include <filesystem>

namespace client::asset {

class FileAssetReader final : public IAssetReader {
public:
    explicit FileAssetReader(std::filesystem::path root);

    std::optional<std::vector<std::uint8_t>> ReadAll(std::string_view path) const override;
    std::optional<std::filesystem::path> RootPath() const override;

private:
    std::filesystem::path Resolve(std::string_view path) const;

    std::filesystem::path m_root;
};

} // namespace client::asset
