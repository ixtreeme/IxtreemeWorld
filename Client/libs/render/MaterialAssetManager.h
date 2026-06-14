#pragma once

#include "AssetDatabase.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

struct MaterialAsset
{
    Guid guid{};
    std::string name;
    std::filesystem::path path;
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normalStrength = 1.0f;
    float aoStrength = 1.0f;
    std::array<float, 4> emissive{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 2> uvTiling{1.0f, 1.0f};
    std::array<float, 2> uvOffset{0.0f, 0.0f};

    enum class AlphaMode
    {
        Opaque,
        Mask,
        Blend
    };
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    std::optional<Guid> baseColorTexture;
    std::optional<Guid> normalTexture;
    std::optional<Guid> metallicRoughnessTexture;
    std::optional<Guid> aoTexture;
    std::optional<Guid> emissiveTexture;
    std::uint64_t loadedFromTimestamp = 0;
};

struct GltfMaterialSource
{
    std::string name;
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normalStrength = 1.0f;
    float aoStrength = 1.0f;
    std::array<float, 4> emissive{0.0f, 0.0f, 0.0f, 0.0f};
    std::string alphaMode = "opaque";
    float alphaCutoff = 0.5f;
    std::filesystem::path baseColorTexturePath;
    std::filesystem::path normalTexturePath;
    std::filesystem::path metallicRoughnessTexturePath;
    std::filesystem::path aoTexturePath;
    std::filesystem::path emissiveTexturePath;
};

class MaterialAssetManager
{
public:
    struct ImportSummary
    {
        std::uint32_t materials = 0;
        std::uint32_t generated = 0;
        std::uint32_t reused = 0;
        std::uint32_t conflicts = 0;
    };

    static MaterialAssetManager& Instance();
    static Guid PinkMissingMaterialGuid();

    explicit MaterialAssetManager(AssetDatabase& db);

    MaterialAsset* getOrLoad(const Guid& guid);
    bool save(const MaterialAsset& material);
    Guid createFromGltfMaterial(const GltfMaterialSource& gltfMat,
                                const std::filesystem::path& materialFolder,
                                const std::string& materialName,
                                ImportSummary* summary = nullptr);
    void invalidate(const Guid& guid);

private:
    AssetDatabase& db_;
    std::unordered_map<Guid, std::unique_ptr<MaterialAsset>> cache_;
};
