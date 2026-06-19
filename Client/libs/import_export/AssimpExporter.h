#pragma once

#include <filesystem>
#include <string>
#include <vector>

class AssimpExporter
{
public:
    struct ExportOptions
    {
        std::filesystem::path outputPath;
        bool embedTextures = true;
        bool exportMaterials = true;
        bool exportSkeletal = true;
        bool exportAnimations = true;
        std::string format = "fbx";
    };

    struct SourceAsset
    {
        std::filesystem::path path;
        std::string nodeName;
        float transform[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
    };

    static bool ExportAssetToFbx(const std::filesystem::path& sourcePath,
                                 const ExportOptions& options,
                                 std::string& error);
    static bool ExportAssetsToFbx(const std::vector<SourceAsset>& sources,
                                  const ExportOptions& options,
                                  std::string& error);
};
