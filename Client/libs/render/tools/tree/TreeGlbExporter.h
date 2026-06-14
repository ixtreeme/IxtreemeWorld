#pragma once

#include <ixtreemetree/tree_mesh.h>

#include <filesystem>
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
};

struct TreeMaterialBinding
{
    std::filesystem::path barkBaseColorTexturePath;
    std::filesystem::path leafBaseColorTexturePath;
    float leafAlphaCutoff = 0.5f;
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
