#include "AssimpImporter.h"

#include "Debug.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/vec_float.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace
{
constexpr unsigned int kFbxImportFlags =
    aiProcess_Triangulate |
    aiProcess_GenSmoothNormals |
    aiProcess_CalcTangentSpace |
    aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights |
    aiProcess_ImproveCacheLocality |
    aiProcess_ValidateDataStructure |
    aiProcess_ConvertToLeftHanded |
    aiProcess_FlipUVs;

std::string SanitizeName(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            c = '_';
    }
    while (!value.empty() && (value.back() == '_' || value.back() == '-'))
        value.pop_back();
    return value.empty() ? "asset" : value;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path ResolveTexturePath(const std::filesystem::path& sourceFolder, const std::string& texturePath)
{
    std::filesystem::path path(texturePath);
    if (path.is_absolute())
        return path;
    return sourceFolder / path;
}

std::filesystem::path UniquePath(const std::filesystem::path& folder, const std::filesystem::path& filename)
{
    const std::string stem = SanitizeName(filename.stem().string());
    std::string ext = filename.extension().string();
    if (ext.empty())
        ext = ".png";
    std::filesystem::path candidate = folder / (stem + ext);
    for (int i = 2; std::filesystem::exists(candidate) && i < 10000; ++i)
        candidate = folder / (stem + "_" + std::to_string(i) + ext);
    return candidate;
}

std::string EmbeddedTextureExtension(const aiTexture& texture)
{
    std::string hint(texture.achFormatHint);
    hint = Lower(hint);
    if (hint.empty())
        hint = "png";
    if (hint == "jpeg")
        hint = "jpg";
    return "." + hint;
}

bool WriteBytes(const std::filesystem::path& path, const void* data, std::size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool WriteRawTextureAsTga(const std::filesystem::path& path, const aiTexture& texture)
{
    if (texture.mHeight == 0 || !texture.pcData)
        return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;

    const std::uint8_t header[18] = {
        0, 0, 2,
        0, 0, 0, 0, 0,
        0, 0,
        0, 0,
        static_cast<std::uint8_t>(texture.mWidth & 0xff),
        static_cast<std::uint8_t>((texture.mWidth >> 8) & 0xff),
        static_cast<std::uint8_t>(texture.mHeight & 0xff),
        static_cast<std::uint8_t>((texture.mHeight >> 8) & 0xff),
        32,
        0x20
    };
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(texture.pcData),
        static_cast<std::streamsize>(texture.mWidth) * static_cast<std::streamsize>(texture.mHeight) * 4);
    return file.good();
}

std::optional<std::filesystem::path> ExtractOrCopyTexture(const aiScene& scene,
                                                          const std::filesystem::path& fbxPath,
                                                          const std::string& texturePath,
                                                          const std::filesystem::path& textureOutputDir,
                                                          std::uint32_t& embedded,
                                                          std::uint32_t& external,
                                                          std::uint32_t& missing)
{
    if (texturePath.empty() || textureOutputDir.empty())
        return std::nullopt;

    if (const aiTexture* embeddedTexture = scene.GetEmbeddedTexture(texturePath.c_str()))
    {
        std::string name = embeddedTexture->mFilename.length > 0
            ? embeddedTexture->mFilename.C_Str()
            : ("embedded_" + std::to_string(embedded));
        std::filesystem::path filename(name);
        if (!filename.has_extension())
            filename += embeddedTexture->mHeight == 0 ? EmbeddedTextureExtension(*embeddedTexture) : ".tga";
        const std::filesystem::path destination = UniquePath(textureOutputDir, filename.filename());
        const bool ok = embeddedTexture->mHeight == 0
            ? WriteBytes(destination, embeddedTexture->pcData, embeddedTexture->mWidth)
            : WriteRawTextureAsTga(destination, *embeddedTexture);
        if (ok)
        {
            ++embedded;
            return destination;
        }
        ++missing;
        TraceError("[FBX-IMPORT] failed to extract embedded texture: %s", texturePath.c_str());
        return std::nullopt;
    }

    const std::filesystem::path source = ResolveTexturePath(fbxPath.parent_path(), texturePath);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || !std::filesystem::is_regular_file(source, ec))
    {
        ++missing;
        TraceError("[FBX-IMPORT] missing external texture: %s", source.generic_string().c_str());
        return std::nullopt;
    }

    const std::filesystem::path destination = UniquePath(textureOutputDir, source.filename());
    std::filesystem::create_directories(destination.parent_path(), ec);
    ec.clear();
    if (!std::filesystem::equivalent(source, destination, ec))
    {
        ec.clear();
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            ++missing;
            TraceError("[FBX-IMPORT] failed to copy external texture: %s error=%s",
                source.generic_string().c_str(),
                ec.message().c_str());
            return std::nullopt;
        }
    }
    ++external;
    return destination;
}

float MetadataFloat(const aiScene& scene, const char* key, float fallback)
{
    if (!scene.mMetaData)
        return fallback;
    float floatValue = fallback;
    if (scene.mMetaData->Get(key, floatValue))
        return floatValue;
    double doubleValue = fallback;
    if (scene.mMetaData->Get(key, doubleValue))
        return static_cast<float>(doubleValue);
    int intValue = static_cast<int>(fallback);
    if (scene.mMetaData->Get(key, intValue))
        return static_cast<float>(intValue);
    return fallback;
}

std::array<float, 16> UpAxisTransform(const aiScene& scene)
{
    const int upAxis = static_cast<int>(MetadataFloat(scene, "UpAxis", 2.0f));
    const int upSign = static_cast<int>(MetadataFloat(scene, "UpAxisSign", 1.0f));
    if (upAxis == 1 && upSign == 1)
        return {1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1};

    const float angle = upSign >= 0 ? -1.5707963267948966f : 1.5707963267948966f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {1, 0, 0, 0,
            0, c, s, 0,
            0, -s, c, 0,
            0, 0, 0, 1};
}

void TransformPoint(const std::array<float, 16>& m, float& x, float& y, float& z)
{
    const float ox = x;
    const float oy = y;
    const float oz = z;
    x = ox * m[0] + oy * m[4] + oz * m[8] + m[12];
    y = ox * m[1] + oy * m[5] + oz * m[9] + m[13];
    z = ox * m[2] + oy * m[6] + oz * m[10] + m[14];
}

void TransformVector(const std::array<float, 16>& m, float& x, float& y, float& z)
{
    const float ox = x;
    const float oy = y;
    const float oz = z;
    x = ox * m[0] + oy * m[4] + oz * m[8];
    y = ox * m[1] + oy * m[5] + oz * m[9];
    z = ox * m[2] + oy * m[6] + oz * m[10];
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 0.0001f)
    {
        x /= len;
        y /= len;
        z /= len;
    }
}

std::filesystem::path TextureFor(aiMaterial& material,
                                 const aiScene& scene,
                                 const std::filesystem::path& fbxPath,
                                 const std::filesystem::path& textureOutputDir,
                                 aiTextureType type,
                                 std::uint32_t& embedded,
                                 std::uint32_t& external,
                                 std::uint32_t& missing)
{
    if (material.GetTextureCount(type) == 0)
        return {};
    aiString texturePath;
    if (material.GetTexture(type, 0, &texturePath) != aiReturn_SUCCESS)
        return {};
    if (textureOutputDir.empty())
    {
        const std::filesystem::path source = ResolveTexturePath(fbxPath.parent_path(), texturePath.C_Str());
        std::error_code ec;
        if (std::filesystem::exists(source, ec) && std::filesystem::is_regular_file(source, ec))
            return source;

        const std::filesystem::path sidecar = fbxPath.parent_path() /
            (SanitizeName(fbxPath.stem().string()) + "_textures") /
            std::filesystem::path(texturePath.C_Str()).filename();
        ec.clear();
        if (std::filesystem::exists(sidecar, ec) && std::filesystem::is_regular_file(sidecar, ec))
            return sidecar;
        return {};
    }
    if (const auto path = ExtractOrCopyTexture(scene, fbxPath, texturePath.C_Str(), textureOutputDir, embedded, external, missing))
        return *path;
    return {};
}

GltfMaterialSource BuildMaterialSource(aiMaterial& material,
                                       const aiScene& scene,
                                       const std::filesystem::path& fbxPath,
                                       const std::filesystem::path& textureOutputDir,
                                       std::uint32_t& embedded,
                                       std::uint32_t& external,
                                       std::uint32_t& missing)
{
    GltfMaterialSource out{};
    aiString name;
    if (material.Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS && name.length > 0)
        out.name = name.C_Str();
    if (out.name.empty())
        out.name = "material";

    aiColor4D base{};
    if (material.Get(AI_MATKEY_BASE_COLOR, base) == aiReturn_SUCCESS ||
        material.Get(AI_MATKEY_COLOR_DIFFUSE, base) == aiReturn_SUCCESS)
    {
        out.baseColor = {base.r, base.g, base.b, base.a};
    }
    float value = 0.0f;
    if (material.Get(AI_MATKEY_METALLIC_FACTOR, value) == aiReturn_SUCCESS)
        out.metallic = value;
    value = 0.5f;
    if (material.Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == aiReturn_SUCCESS)
        out.roughness = value;
    value = 1.0f;
    if (material.Get(AI_MATKEY_OPACITY, value) == aiReturn_SUCCESS)
    {
        out.baseColor[3] = value;
        if (value < 0.99f)
            out.alphaMode = "blend";
    }

    out.baseColorTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_BASE_COLOR, embedded, external, missing);
    if (out.baseColorTexturePath.empty())
        out.baseColorTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_DIFFUSE, embedded, external, missing);
    out.normalTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_NORMALS, embedded, external, missing);
    if (out.normalTexturePath.empty())
        out.normalTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_HEIGHT, embedded, external, missing);
    out.metallicRoughnessTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_GLTF_METALLIC_ROUGHNESS, embedded, external, missing);
    if (out.metallicRoughnessTexturePath.empty())
        out.metallicRoughnessTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_METALNESS, embedded, external, missing);
    if (out.metallicRoughnessTexturePath.empty())
        out.metallicRoughnessTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_DIFFUSE_ROUGHNESS, embedded, external, missing);
    out.aoTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_AMBIENT_OCCLUSION, embedded, external, missing);
    out.emissiveTexturePath = TextureFor(material, scene, fbxPath, textureOutputDir, aiTextureType_EMISSIVE, embedded, external, missing);

    return out;
}

std::array<float, 16> IdentityMatrix()
{
    return {1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

std::array<float, 16> MatrixToRowMajor(const aiMatrix4x4& matrix)
{
    return {matrix.a1, matrix.a2, matrix.a3, matrix.a4,
        matrix.b1, matrix.b2, matrix.b3, matrix.b4,
        matrix.c1, matrix.c2, matrix.c3, matrix.c4,
        matrix.d1, matrix.d2, matrix.d3, matrix.d4};
}

AssimpImporter::AnimationKeyframe DecomposeTransform(const aiMatrix4x4& matrix, float time)
{
    aiVector3D scale;
    aiQuaternion rotation;
    aiVector3D translation;
    matrix.Decompose(scale, rotation, translation);

    AssimpImporter::AnimationKeyframe key{};
    key.time = time;
    key.position[0] = translation.x;
    key.position[1] = translation.y;
    key.position[2] = translation.z;
    key.rotation[0] = rotation.x;
    key.rotation[1] = rotation.y;
    key.rotation[2] = rotation.z;
    key.rotation[3] = rotation.w;
    key.scale[0] = scale.x;
    key.scale[1] = scale.y;
    key.scale[2] = scale.z;
    return key;
}

std::unordered_map<std::string, int> BuildBoneIndexMap(const AssimpImporter::SkeletonData& skeleton)
{
    std::unordered_map<std::string, int> indices;
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
        indices.emplace(skeleton.bones[i].name, static_cast<int>(i));
    return indices;
}

void ExtractSkeleton(const aiScene& scene, AssimpImporter::ImportResult& result)
{
    std::unordered_map<std::string, std::array<float, 16>> inverseBindByName;
    std::unordered_map<std::string, bool> boneNames;
    for (unsigned int meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex)
    {
        const aiMesh* mesh = scene.mMeshes[meshIndex];
        if (!mesh)
            continue;
        for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
        {
            const aiBone* bone = mesh->mBones[boneIndex];
            if (!bone)
                continue;
            const std::string name = bone->mName.C_Str();
            boneNames[name] = true;
            inverseBindByName[name] = MatrixToRowMajor(bone->mOffsetMatrix);
        }
    }
    if (boneNames.empty())
        return;

    AssimpImporter::SkeletonData skeleton;
    skeleton.bones.reserve(boneNames.size());
    std::unordered_map<std::string, int> created;
    auto walk = [&](auto&& self, const aiNode* node, int parentIndex) -> void {
        if (!node)
            return;
        const std::string nodeName = node->mName.C_Str();
        int currentParent = parentIndex;
        if (boneNames.contains(nodeName))
        {
            AssimpImporter::BoneData bone{};
            bone.name = nodeName;
            bone.parentIndex = parentIndex;
            bone.localTransform = MatrixToRowMajor(node->mTransformation);
            auto inverse = inverseBindByName.find(nodeName);
            bone.inverseBindPose = inverse != inverseBindByName.end() ? inverse->second : IdentityMatrix();
            currentParent = static_cast<int>(skeleton.bones.size());
            created[nodeName] = currentParent;
            skeleton.bones.push_back(std::move(bone));
        }
        for (unsigned int child = 0; child < node->mNumChildren; ++child)
            self(self, node->mChildren[child], currentParent);
    };
    walk(walk, scene.mRootNode, -1);

    if (skeleton.bones.empty())
        return;

    if (skeleton.bones.size() < boneNames.size())
    {
        for (const auto& [name, _] : boneNames)
        {
            if (created.contains(name))
                continue;
            AssimpImporter::BoneData bone{};
            bone.name = name;
            bone.parentIndex = -1;
            bone.localTransform = IdentityMatrix();
            auto inverse = inverseBindByName.find(name);
            bone.inverseBindPose = inverse != inverseBindByName.end() ? inverse->second : IdentityMatrix();
            skeleton.bones.push_back(std::move(bone));
        }
    }

    result.skeleton = std::move(skeleton);
    result.hasSkeletal = true;
    result.skeletalIgnored = false;
    Tracenf("[FBX-IMPORT] skeletal_detected bones=%zu", result.skeleton->bones.size());
}

std::uint8_t WeightToByte(float weight)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(weight, 0.0f, 1.0f) * 255.0f));
}

void ExtractSkinning(const aiMesh& mesh,
                     int outputMeshIndex,
                     const std::unordered_map<std::string, int>& boneIndexByName,
                     AssimpImporter::ImportResult& result)
{
    if (!mesh.HasBones() || outputMeshIndex < 0)
        return;

    struct Influence
    {
        int bone = 0;
        float weight = 0.0f;
    };
    std::vector<std::vector<Influence>> perVertex(mesh.mNumVertices);
    for (unsigned int boneIndex = 0; boneIndex < mesh.mNumBones; ++boneIndex)
    {
        const aiBone* bone = mesh.mBones[boneIndex];
        if (!bone)
            continue;
        const auto mapped = boneIndexByName.find(bone->mName.C_Str());
        if (mapped == boneIndexByName.end())
            continue;
        for (unsigned int weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex)
        {
            const aiVertexWeight& weight = bone->mWeights[weightIndex];
            if (weight.mVertexId < perVertex.size())
                perVertex[weight.mVertexId].push_back({mapped->second, weight.mWeight});
        }
    }

    AssimpImporter::SkinningData skin{};
    skin.meshIndex = outputMeshIndex;
    skin.influences.resize(mesh.mNumVertices);
    for (std::size_t vertex = 0; vertex < perVertex.size(); ++vertex)
    {
        auto& influences = perVertex[vertex];
        std::sort(influences.begin(), influences.end(), [](const Influence& a, const Influence& b) {
            return a.weight > b.weight;
        });
        if (influences.size() > 4)
            influences.resize(4);
        float sum = 0.0f;
        for (const Influence& influence : influences)
            sum += std::max(0.0f, influence.weight);
        if (sum <= 0.0f)
            continue;
        for (std::size_t i = 0; i < influences.size(); ++i)
        {
            skin.influences[vertex].boneIndices[i] = static_cast<std::uint8_t>(std::clamp(influences[i].bone, 0, 255));
            skin.influences[vertex].boneWeights[i] = WeightToByte(influences[i].weight / sum);
        }
    }
    result.skinning.push_back(std::move(skin));
}

const aiVectorKey* FindVectorKey(const aiVectorKey* keys, unsigned int count, double tick)
{
    if (!keys || count == 0)
        return nullptr;
    const aiVectorKey* best = &keys[0];
    for (unsigned int i = 1; i < count; ++i)
    {
        if (keys[i].mTime > tick)
            break;
        best = &keys[i];
    }
    return best;
}

const aiQuatKey* FindQuatKey(const aiQuatKey* keys, unsigned int count, double tick)
{
    if (!keys || count == 0)
        return nullptr;
    const aiQuatKey* best = &keys[0];
    for (unsigned int i = 1; i < count; ++i)
    {
        if (keys[i].mTime > tick)
            break;
        best = &keys[i];
    }
    return best;
}

void ExtractAnimations(const aiScene& scene,
                       const std::filesystem::path& fbxPath,
                       const AssimpImporter::SkeletonData& skeleton,
                       AssimpImporter::ImportResult& result)
{
    if (scene.mNumAnimations == 0)
        return;
    const std::unordered_map<std::string, int> boneIndexByName = BuildBoneIndexMap(skeleton);
    for (unsigned int animationIndex = 0; animationIndex < scene.mNumAnimations; ++animationIndex)
    {
        const aiAnimation* aiAnim = scene.mAnimations[animationIndex];
        if (!aiAnim)
            continue;
        const double ticksPerSecond = aiAnim->mTicksPerSecond > 0.0 ? aiAnim->mTicksPerSecond : 25.0;
        AssimpImporter::AnimationData animation{};
        animation.name = aiAnim->mName.length > 0
            ? aiAnim->mName.C_Str()
            : (fbxPath.stem().string() + "_anim_" + std::to_string(animationIndex));
        animation.ticksPerSecond = static_cast<float>(ticksPerSecond);
        animation.duration = static_cast<float>(aiAnim->mDuration / ticksPerSecond);

        for (unsigned int channelIndex = 0; channelIndex < aiAnim->mNumChannels; ++channelIndex)
        {
            const aiNodeAnim* channel = aiAnim->mChannels[channelIndex];
            if (!channel)
                continue;
            const auto boneIt = boneIndexByName.find(channel->mNodeName.C_Str());
            if (boneIt == boneIndexByName.end())
                continue;

            std::set<double> ticks;
            for (unsigned int i = 0; i < channel->mNumPositionKeys; ++i)
                ticks.insert(channel->mPositionKeys[i].mTime);
            for (unsigned int i = 0; i < channel->mNumRotationKeys; ++i)
                ticks.insert(channel->mRotationKeys[i].mTime);
            for (unsigned int i = 0; i < channel->mNumScalingKeys; ++i)
                ticks.insert(channel->mScalingKeys[i].mTime);
            if (ticks.empty())
                ticks.insert(0.0);

            AssimpImporter::BoneTrack track{};
            track.boneIndex = boneIt->second;
            track.keyframes.reserve(ticks.size());
            for (double tick : ticks)
            {
                AssimpImporter::AnimationKeyframe key{};
                key.time = static_cast<float>(tick / ticksPerSecond);
                if (const aiVectorKey* position = FindVectorKey(channel->mPositionKeys, channel->mNumPositionKeys, tick))
                {
                    key.position[0] = position->mValue.x;
                    key.position[1] = position->mValue.y;
                    key.position[2] = position->mValue.z;
                }
                if (const aiQuatKey* rotation = FindQuatKey(channel->mRotationKeys, channel->mNumRotationKeys, tick))
                {
                    key.rotation[0] = rotation->mValue.x;
                    key.rotation[1] = rotation->mValue.y;
                    key.rotation[2] = rotation->mValue.z;
                    key.rotation[3] = rotation->mValue.w;
                }
                if (const aiVectorKey* scale = FindVectorKey(channel->mScalingKeys, channel->mNumScalingKeys, tick))
                {
                    key.scale[0] = scale->mValue.x;
                    key.scale[1] = scale->mValue.y;
                    key.scale[2] = scale->mValue.z;
                }
                track.keyframes.push_back(key);
            }
            animation.tracks.push_back(std::move(track));
        }

        if (!animation.tracks.empty())
            result.animations.push_back(std::move(animation));
    }
}

ozz::math::Transform OzzTransformFromMatrix(const std::array<float, 16>& matrix)
{
    aiMatrix4x4 aiMatrix(
        matrix[0], matrix[1], matrix[2], matrix[3],
        matrix[4], matrix[5], matrix[6], matrix[7],
        matrix[8], matrix[9], matrix[10], matrix[11],
        matrix[12], matrix[13], matrix[14], matrix[15]);
    const AssimpImporter::AnimationKeyframe key = DecomposeTransform(aiMatrix, 0.0f);
    ozz::math::Transform transform{};
    transform.translation = ozz::math::Float3(key.position[0], key.position[1], key.position[2]);
    transform.rotation = ozz::math::Quaternion(key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]);
    transform.scale = ozz::math::Float3(key.scale[0], key.scale[1], key.scale[2]);
    return transform;
}

void FillRawJoint(const AssimpImporter::SkeletonData& skeleton,
                  int boneIndex,
                  ozz::animation::offline::RawSkeleton::Joint& joint)
{
    const AssimpImporter::BoneData& bone = skeleton.bones[boneIndex];
    joint.name = bone.name.c_str();
    joint.transform = OzzTransformFromMatrix(bone.localTransform);

    std::vector<int> children;
    for (int i = 0; i < static_cast<int>(skeleton.bones.size()); ++i)
    {
        if (skeleton.bones[i].parentIndex == boneIndex)
            children.push_back(i);
    }
    joint.children.resize(children.size());
    for (std::size_t i = 0; i < children.size(); ++i)
        FillRawJoint(skeleton, children[i], joint.children[i]);
}

bool SaveOzzObject(const std::filesystem::path& path, const auto& object, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ozz::io::File file(path.string().c_str(), "wb");
    if (!file.opened())
    {
        error = "failed to open output archive: " + path.generic_string();
        return false;
    }
    ozz::io::OArchive archive(&file);
    archive << object;
    return true;
}
}

AssimpImporter::ImportResult AssimpImporter::importFile(const std::filesystem::path& fbxPath,
                                                        const ImportOptions& options) const
{
    ImportResult result{};
    result.assetName = fbxPath.stem().string();

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(fbxPath.string(), kFbxImportFlags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        result.errorMessage = importer.GetErrorString();
        if (result.errorMessage.empty())
            result.errorMessage = "Assimp returned incomplete scene";
        return result;
    }

    const std::array<float, 16> axisTransform = UpAxisTransform(*scene);
    result.skeletalIgnored = scene->HasAnimations();
    ExtractSkeleton(*scene, result);
    std::unordered_map<std::string, int> boneIndexByName;
    if (result.skeleton)
    {
        boneIndexByName = BuildBoneIndexMap(*result.skeleton);
        ExtractAnimations(*scene, fbxPath, *result.skeleton, result);
    }

    result.materials.reserve(std::max(1u, scene->mNumMaterials));
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
    {
        if (!scene->mMaterials[i])
            continue;
        result.materials.push_back(BuildMaterialSource(*scene->mMaterials[i],
            *scene,
            fbxPath,
            options.extractTextures ? options.textureOutputDir : std::filesystem::path{},
            result.embeddedTextures,
            result.externalTextures,
            result.missingTextures));
    }
    if (result.materials.empty())
        result.materials.push_back(GltfMaterialSource{});

    result.meshes.reserve(scene->mNumMeshes);
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (!mesh || mesh->mNumVertices == 0 || mesh->mNumFaces == 0 || !mesh->HasPositions())
            continue;

        StaticMeshData outMesh{};
        outMesh.name = mesh->mName.length > 0 ? mesh->mName.C_Str() : ("mesh_" + std::to_string(meshIndex));
        outMesh.materialIndex = mesh->mMaterialIndex < result.materials.size() ? mesh->mMaterialIndex : 0u;
        outMesh.vertices.resize(mesh->mNumVertices);
        for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
        {
            Vertex& vertex = outMesh.vertices[vertexIndex];
            vertex.position[0] = mesh->mVertices[vertexIndex].x;
            vertex.position[1] = mesh->mVertices[vertexIndex].y;
            vertex.position[2] = mesh->mVertices[vertexIndex].z;
            TransformPoint(axisTransform, vertex.position[0], vertex.position[1], vertex.position[2]);

            if (mesh->HasNormals())
            {
                vertex.normal[0] = mesh->mNormals[vertexIndex].x;
                vertex.normal[1] = mesh->mNormals[vertexIndex].y;
                vertex.normal[2] = mesh->mNormals[vertexIndex].z;
                TransformVector(axisTransform, vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            }
            if (mesh->HasTextureCoords(0))
            {
                vertex.uv[0] = mesh->mTextureCoords[0][vertexIndex].x;
                vertex.uv[1] = mesh->mTextureCoords[0][vertexIndex].y;
            }
        }

        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3)
                continue;
            outMesh.indices.push_back(face.mIndices[0]);
            outMesh.indices.push_back(face.mIndices[1]);
            outMesh.indices.push_back(face.mIndices[2]);
        }
        if (!outMesh.indices.empty())
        {
            const int outputMeshIndex = static_cast<int>(result.meshes.size());
            result.meshes.push_back(std::move(outMesh));
            if (result.skeleton)
                ExtractSkinning(*mesh, outputMeshIndex, boneIndexByName, result);
        }
    }

    if (result.meshes.empty())
    {
        result.errorMessage = "no static mesh data extracted";
        return result;
    }

    result.success = true;
    Tracenf("[FBX-IMPORT] parsed path=%s meshes=%zu materials=%zu embeddedTextures=%u externalTextures=%u missingTextures=%u bones=%zu animations=%zu hasSkeletal=%s skeletalIgnored=%s",
        fbxPath.generic_string().c_str(),
        result.meshes.size(),
        result.materials.size(),
        result.embeddedTextures,
        result.externalTextures,
        result.missingTextures,
        result.skeleton ? result.skeleton->bones.size() : 0u,
        result.animations.size(),
        result.hasSkeletal ? "yes" : "no",
        result.skeletalIgnored ? "yes" : "no");
    return result;
}

bool AssimpImporter::writeOzzSidecars(const ImportResult& result,
                                      const std::filesystem::path& skeletonPath,
                                      const std::vector<std::filesystem::path>& animationPaths,
                                      std::string& error) const
{
    if (!result.skeleton || result.skeleton->bones.empty())
    {
        error = "FBX does not contain a skeleton";
        return false;
    }

    ozz::animation::offline::RawSkeleton rawSkeleton;
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(result.skeleton->bones.size()); ++i)
    {
        if (result.skeleton->bones[i].parentIndex < 0)
            roots.push_back(i);
    }
    rawSkeleton.roots.resize(roots.size());
    for (std::size_t i = 0; i < roots.size(); ++i)
        FillRawJoint(*result.skeleton, roots[i], rawSkeleton.roots[i]);

    if (!rawSkeleton.Validate())
    {
        error = "ozz RawSkeleton validation failed";
        return false;
    }

    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton = skeletonBuilder(rawSkeleton);
    if (!skeleton)
    {
        error = "ozz SkeletonBuilder failed";
        return false;
    }
    if (!SaveOzzObject(skeletonPath, *skeleton, error))
        return false;

    std::size_t animationCount = std::min(animationPaths.size(), result.animations.size());
    for (std::size_t animIndex = 0; animIndex < animationCount; ++animIndex)
    {
        const AnimationData& source = result.animations[animIndex];
        if (source.duration <= 0.0f)
            continue;

        ozz::animation::offline::RawAnimation rawAnimation;
        rawAnimation.name = source.name.c_str();
        rawAnimation.duration = std::max(0.001f, source.duration);
        rawAnimation.tracks.resize(result.skeleton->bones.size());
        for (const BoneTrack& track : source.tracks)
        {
            if (track.boneIndex < 0 || track.boneIndex >= static_cast<int>(rawAnimation.tracks.size()))
                continue;
            auto& rawTrack = rawAnimation.tracks[track.boneIndex];
            float previousTime = -1.0f;
            for (const AnimationKeyframe& key : track.keyframes)
            {
                float time = std::clamp(key.time, 0.0f, rawAnimation.duration);
                if (time <= previousTime)
                    time = std::min(rawAnimation.duration, previousTime + 0.0001f);
                if (time > rawAnimation.duration)
                    break;
                previousTime = time;
                rawTrack.translations.push_back({time, ozz::math::Float3(key.position[0], key.position[1], key.position[2])});
                rawTrack.rotations.push_back({time, ozz::math::Quaternion(key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3])});
                rawTrack.scales.push_back({time, ozz::math::Float3(key.scale[0], key.scale[1], key.scale[2])});
            }
        }
        if (!rawAnimation.Validate())
        {
            Tracenf("[FBX-IMPORT] animation skipped name=%s reason=raw_animation_validation_failed",
                source.name.c_str());
            continue;
        }
        ozz::animation::offline::AnimationBuilder animationBuilder;
        ozz::unique_ptr<ozz::animation::Animation> animation = animationBuilder(rawAnimation);
        if (!animation)
        {
            Tracenf("[FBX-IMPORT] animation skipped name=%s reason=AnimationBuilder_failed",
                source.name.c_str());
            continue;
        }
        if (!SaveOzzObject(animationPaths[animIndex], *animation, error))
            return false;
    }

    Tracenf("[FBX-IMPORT] ozz sidecars written skeleton=%s animations=%zu",
        skeletonPath.generic_string().c_str(),
        animationCount);
    return true;
}
