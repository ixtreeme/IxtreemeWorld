#pragma once

#include "AssetDatabase.h"

#include <ixtreemetree/tree_mesh.h>

#include <filesystem>
#include <optional>
#include <string>

namespace tree_tool
{
struct TreeExportResult
{
    bool ok = false;
    std::filesystem::path modelPath;
    std::filesystem::path materialFolder;
    std::string error;
    float durationMs = 0.0f;
    float leafAreaReduction = 0.0f;  // the share of the leaf cards' area trimmed away (0..1)
};

struct TreeMaterialBinding
{
    // The materials the tree is saved with (its default materials). Without one, a material is made
    // from the built-in texture below and saved beside the model.
    std::optional<Guid> barkMaterial;
    std::optional<Guid> leafMaterial;
    std::filesystem::path barkBaseColorTexturePath;
    std::filesystem::path leafBaseColorTexturePath;
    float leafAlphaCutoff = 0.5f;  // for a made leaf material
    bool trimTransparentLeafBorders = true;  // see trimLeafCards
};

class TreeGlbExporter
{
public:
    static bool IsValidAssetName(const std::string& name, std::string* error = nullptr);
    static TreeExportResult SaveAsAsset(const ixtreemetree::TreeMesh& mesh,
                                        const std::filesystem::path& projectRoot,
                                        const std::string& assetName,
                                        const TreeMaterialBinding& materialBinding = {});
};
}
