#pragma once

#include "asset/IAssetReader.h"

#include <string>

struct AAssetManager;

namespace client::asset {

class AAssetManagerAssetReader final : public IAssetReader {
public:
    explicit AAssetManagerAssetReader(AAssetManager* manager);

    std::optional<std::vector<std::uint8_t>> ReadAll(std::string_view path) const override;

private:
    void DumpAssetDirectory(const std::string& directory) const;

    AAssetManager* m_manager = nullptr;
};

} // namespace client::asset
