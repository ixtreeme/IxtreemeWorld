#pragma once

#include "AssetDatabase.h"
#include "asset/IAssetReader.h"
#include <ixtreemetree/tree_mesh.h>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct MaterialAsset;

namespace tree_tool
{
struct MeshImpostorPart
{
    std::span<const ixtreemetree::Vertex> vertices;
    std::span<const std::uint32_t> indices;
};
struct TreeImpostorSettings
{
    bool enabled = true;
    int resolution = 256;
    int azimuths = 8;
    float distance = 180.0f;
    float transition = 30.0f;
};
struct TreeImpostorData
{
    std::string billboardPath;
    std::array<float, 3> center{};
    float radius = 0.0f;
    float distance = 180.0f;
    float transition = 30.0f;
    int azimuths = 8;
    int elevations = 3;
    int resolution = 256;
    std::vector<std::string> materials;
    std::vector<std::string> materialSignatures;
    std::vector<std::string> sourcePaths;
    std::vector<std::string> sourceHashes;
};
struct TreeImpostorView
{
    int first = 0, second = 0, row = 1;
    float blend = 0.0f;
};
// Yaw/elevation of the camera relative to the original tree, in radians.
TreeImpostorView SelectTreeImpostorView(float azimuth, float elevation, int azimuths);
float TreeImpostorWeight(float distance, float begin, float transition);
std::array<float, 4> TreeImpostorUv(int column, int row, int azimuths, int resolution);
std::optional<TreeImpostorData> LoadTreeImpostor(const client::asset::IAssetReader& assets,
                                                 const std::string& modelPath, std::string& error);
bool ValidateTreeImpostor(const client::asset::IAssetReader& assets, const TreeImpostorData& data);
// Only render-relevant values: live material edits invalidate a bake without reading atlas files.
std::string TreeImpostorMaterialSignature(const MaterialAsset& material);
bool BakeTreeImpostor(const ixtreemetree::TreeMesh& mesh, const std::filesystem::path& modelPath,
                      const std::vector<Guid>& materials, const TreeImpostorSettings& settings,
                      std::vector<Guid>& dependencies, std::string& error);
bool BakeMeshImpostor(std::span<const MeshImpostorPart> parts, const std::filesystem::path& modelPath,
    const std::vector<Guid>& materials, const TreeImpostorSettings& settings,
    std::vector<Guid>& dependencies, std::string& error);
} // namespace tree_tool
