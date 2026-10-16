#pragma once

#include "AssetDatabase.h"

#include <ixtreemetree/tree_options.h>

#include <array>
#include <filesystem>
#include <optional>

namespace tree_tool
{
struct TreePaletteTexture
{
    Guid guid{};
    std::filesystem::path path;
    std::array<float, 4> previewColor{1.0f, 1.0f, 1.0f, 1.0f};
    int gridX = 2;
    int gridY = 2;
};

class TreeTexturePalette
{
public:
    static TreeTexturePalette& Instance();

    void EnsureLoaded(const std::filesystem::path& internalRoot);
    const TreePaletteTexture& Bark(ixtreemetree::BarkType type) const;
    const TreePaletteTexture& Leaf(ixtreemetree::LeafType type) const;

private:
    void Load(const std::filesystem::path& internalRoot);

    bool loaded_ = false;
    std::filesystem::path loadedRoot_;
    std::array<TreePaletteTexture, ixtreemetree::kBarkTypeCount> bark_{};
    std::array<TreePaletteTexture, ixtreemetree::kLeafTypeCount> leaves_{};
};
}
