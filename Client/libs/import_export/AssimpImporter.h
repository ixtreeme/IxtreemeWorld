#pragma once

#include "MaterialAssetManager.h"
#include "math/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class AssimpImporter
{
public:
    struct Vertex
    {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float normal[3] = {0.0f, 1.0f, 0.0f};
        float uv[2] = {0.0f, 0.0f};
    };

    struct StaticMeshData
    {
        std::string name;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::uint32_t materialIndex = 0;
    };

    struct BoneData
    {
        std::string name;
        int parentIndex = -1;
        ixtreeme::math::Mat4 localTransform{};
        ixtreeme::math::Mat4 inverseBindPose{};
    };

    struct SkeletonData
    {
        std::vector<BoneData> bones;
    };

    struct AnimationKeyframe
    {
        float time = 0.0f;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
    };

    struct BoneTrack
    {
        int boneIndex = -1;
        std::vector<AnimationKeyframe> keyframes;
    };

    struct AnimationData
    {
        std::string name;
        float duration = 0.0f;
        float ticksPerSecond = 0.0f;
        std::vector<BoneTrack> tracks;
    };

    struct VertexBoneInfluence
    {
        std::uint8_t boneIndices[4] = {0, 0, 0, 0};
        std::uint8_t boneWeights[4] = {0, 0, 0, 0};
    };

    struct SkinningData
    {
        int meshIndex = -1;
        std::vector<VertexBoneInfluence> influences;
    };

    struct ImportResult
    {
        bool success = false;
        std::string errorMessage;
        std::string assetName;
        std::vector<StaticMeshData> meshes;
        std::vector<GltfMaterialSource> materials;
        std::optional<SkeletonData> skeleton;
        std::vector<AnimationData> animations;
        std::vector<SkinningData> skinning;
        std::uint32_t embeddedTextures = 0;
        std::uint32_t externalTextures = 0;
        std::uint32_t missingTextures = 0;
        bool skeletalIgnored = false;
        bool hasSkeletal = false;
    };

    struct ImportOptions
    {
        std::filesystem::path textureOutputDir;
        bool extractTextures = false;
    };

    ImportResult importFile(const std::filesystem::path& fbxPath,
                            const ImportOptions& options = {}) const;
    bool writeOzzSidecars(const ImportResult& result,
                          const std::filesystem::path& skeletonPath,
                          const std::vector<std::filesystem::path>& animationPaths,
                          std::string& error) const;
};
