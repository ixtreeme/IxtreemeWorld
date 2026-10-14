#include "AssimpImporter.h"

#include "Debug.h"
#include "math/IXMath.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
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
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
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
namespace xm = ixtreeme::math;

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

std::filesystem::path PortableTexturePath(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return std::filesystem::path(value);
}

bool SameBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) != bytes.size()) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> existing((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return existing == bytes;
}

// Preserve edited assets, reuse identical imports, and separate different files sharing a basename.
std::filesystem::path TextureDestination(const std::filesystem::path& folder, const std::filesystem::path& filename,
                                        const std::vector<std::uint8_t>& bytes)
{
    const std::string stem = SanitizeName(filename.stem().string());
    const std::string ext = filename.has_extension() ? filename.extension().string() : ".png";
    for (int i = 1; i < 10000; ++i)
    {
        const auto candidate = folder / (stem + (i == 1 ? "" : "_" + std::to_string(i)) + ext);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || SameBytes(candidate, bytes)) return candidate;
    }
    return {};
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

struct TextureImportContext
{
    const aiScene& scene;
    const std::filesystem::path& model;
    const AssimpImporter::ImportOptions& options;
    AssimpImporter::ImportResult& result;
    std::unordered_map<std::string, std::filesystem::path> resolved;
    std::unordered_map<std::string, std::vector<std::filesystem::path>> byFilename;
    bool indexed = false;

    std::filesystem::path FindExternal(const std::string& reference)
    {
        const auto path = PortableTexturePath(reference);
        const auto root = options.textureSourceDir.empty() ? model.parent_path() : options.textureSourceDir;
        const auto nativeFolder = model.parent_path() / (SanitizeName(model.stem().string()) + "_textures");
        std::error_code ec;
        const auto exists = [&](const std::filesystem::path& candidate) {
            ec.clear(); return std::filesystem::is_regular_file(candidate, ec);
        };
        // The original folder is essential: the destination contains only the copied FBX at this point.
        for (const auto& candidate : {path.is_absolute() ? path : root / path, root / path.filename(),
                model.parent_path() / path, nativeFolder / path.filename(),
                nativeFolder / (SanitizeName(path.stem().string()) + path.extension().string())})
            if (exists(candidate)) return candidate;
        if (!options.extractTextures) return {};
        if (!indexed)
        {
            indexed = true;
            std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code itemError;
                if (it->is_regular_file(itemError)) byFilename[Lower(it->path().filename().string())].push_back(it->path());
            }
        }
        const auto found = byFilename.find(Lower(path.filename().string()));
        // Never guess between two textures with the same filename in a downloaded package.
        return found != byFilename.end() && found->second.size() == 1 ? found->second.front() : std::filesystem::path{};
    }

    std::filesystem::path Get(const std::string& reference)
    {
        const aiTexture* embeddedTexture = scene.GetEmbeddedTexture(reference.c_str());
        std::filesystem::path source;
        if (!embeddedTexture) source = FindExternal(reference);
        else if (!options.extractTextures)
        {
            auto filename = PortableTexturePath(embeddedTexture->mFilename.length ? embeddedTexture->mFilename.C_Str() : reference);
            if (embeddedTexture->mHeight) filename.replace_extension(".tga");
            else if (!filename.has_extension()) filename += EmbeddedTextureExtension(*embeddedTexture);
            source = FindExternal(filename.generic_string());
        }
        std::error_code ec;
        const std::string key = embeddedTexture
            ? "embedded:" + std::to_string(reinterpret_cast<std::uintptr_t>(embeddedTexture))
            : (source.empty() ? "missing:" + reference : std::filesystem::weakly_canonical(source, ec).generic_string());
        if (const auto found = resolved.find(key); found != resolved.end()) return found->second;
        const auto missing = [&]() -> std::filesystem::path {
            resolved[key] = std::filesystem::path{};
            ++result.missingTextures;
            result.missingTexturePaths.push_back(reference);
            TraceError("[FBX-IMPORT] missing/unreadable texture: %s", reference.c_str());
            return {};
        };
        if (!options.extractTextures)
        {
            resolved[key] = source;
            return source;
        }
        if (!embeddedTexture && source.empty()) return missing();
        std::filesystem::path filename;
        std::vector<std::uint8_t> bytes;
        if (embeddedTexture)
        {
            filename = PortableTexturePath(embeddedTexture->mFilename.length ? embeddedTexture->mFilename.C_Str() : reference).filename();
            if (embeddedTexture->mHeight == 0)
            {
                if (!embeddedTexture->pcData || !embeddedTexture->mWidth) return missing();
                const auto* begin = reinterpret_cast<const std::uint8_t*>(embeddedTexture->pcData);
                bytes.assign(begin, begin + embeddedTexture->mWidth);
                if (!filename.has_extension()) filename += EmbeddedTextureExtension(*embeddedTexture);
            }
            else
            {
                // Uncompressed aiTexel is BGRA; write a top-origin 32-bit TGA with alpha bits.
                if (!embeddedTexture->pcData || embeddedTexture->mWidth > 65535 || embeddedTexture->mHeight > 65535) return missing();
                filename.replace_extension(".tga");
                bytes.assign(18, 0);
                bytes[2] = 2;
                bytes[12] = static_cast<std::uint8_t>(embeddedTexture->mWidth);
                bytes[13] = static_cast<std::uint8_t>(embeddedTexture->mWidth >> 8);
                bytes[14] = static_cast<std::uint8_t>(embeddedTexture->mHeight);
                bytes[15] = static_cast<std::uint8_t>(embeddedTexture->mHeight >> 8);
                bytes[16] = 32; bytes[17] = 0x28;
                const auto* begin = reinterpret_cast<const std::uint8_t*>(embeddedTexture->pcData);
                bytes.insert(bytes.end(), begin, begin + static_cast<std::size_t>(embeddedTexture->mWidth) * embeddedTexture->mHeight * 4u);
            }
        }
        else
        {
            filename = source.filename();
            std::ifstream file(source, std::ios::binary);
            if (!file) return missing();
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (bytes.empty()) return missing();
        }
        const auto destination = TextureDestination(options.textureOutputDir, filename, bytes);
        if (destination.empty() || (!SameBytes(destination, bytes) && !WriteBytes(destination, bytes.data(), bytes.size()))) return missing();
        resolved[key] = destination;
        result.texturePaths.push_back(destination);
        if (embeddedTexture) ++result.embeddedTextures; else ++result.externalTextures;
        return destination;
    }
};

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

xm::Mat4 UpAxisTransform(const aiScene& scene)
{
    const int upAxis = static_cast<int>(MetadataFloat(scene, "UpAxis", 2.0f));
    const int upSign = static_cast<int>(MetadataFloat(scene, "UpAxisSign", 1.0f));
    if (upAxis == 1 && upSign == 1)
        return xm::Mat4Identity();

    const float angle = upSign >= 0 ? -xm::HalfPi : xm::HalfPi;
    const float c = xm::Cos(angle);
    const float s = xm::Sin(angle);
    return xm::Mat4{{1, 0, 0, 0,
                     0, c, s, 0,
                     0, -s, c, 0,
                     0, 0, 0, 1}};
}

void ApplyPointTransform(const xm::Mat4& transform, float& x, float& y, float& z)
{
    const xm::Vec3 transformed = xm::TransformPoint(transform, {x, y, z});
    x = transformed.x;
    y = transformed.y;
    z = transformed.z;
}

void ApplyVectorTransform(const xm::Mat4& transform, float& x, float& y, float& z)
{
    const xm::Vec3 transformed = xm::SafeNormalize(xm::TransformVector(transform, {x, y, z}), {x, y, z});
    x = transformed.x;
    y = transformed.y;
    z = transformed.z;
}

std::filesystem::path TextureFor(aiMaterial& material, TextureImportContext& textures, aiTextureType type)
{
    aiString path;
    return material.GetTexture(type, 0, &path) == aiReturn_SUCCESS ? textures.Get(path.C_Str()) : std::filesystem::path{};
}

GltfMaterialSource BuildMaterialSource(aiMaterial& material, TextureImportContext& textures)
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
    const bool hasMetallicFactor = material.Get(AI_MATKEY_METALLIC_FACTOR, value) == aiReturn_SUCCESS;
    if (hasMetallicFactor)
        out.metallic = value;
    value = 0.5f;
    const bool hasRoughnessFactor = material.Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == aiReturn_SUCCESS;
    if (hasRoughnessFactor)
        out.roughness = value;
    value = 1.0f;
    if (material.Get(AI_MATKEY_OPACITY, value) == aiReturn_SUCCESS)
    {
        out.baseColor[3] = value;
        if (value < 0.99f)
            out.alphaMode = "blend";
    }

    out.baseColorTexturePath = TextureFor(material, textures, aiTextureType_BASE_COLOR);
    if (out.baseColorTexturePath.empty())
        out.baseColorTexturePath = TextureFor(material, textures, aiTextureType_DIFFUSE);
    out.normalTexturePath = TextureFor(material, textures, aiTextureType_NORMALS);
    if (out.normalTexturePath.empty())
        out.normalTexturePath = TextureFor(material, textures, aiTextureType_NORMAL_CAMERA);
    out.metallicRoughnessTexturePath = TextureFor(material, textures, aiTextureType_GLTF_METALLIC_ROUGHNESS);
    out.metallicTexturePath = TextureFor(material, textures, aiTextureType_METALNESS);
    out.roughnessTexturePath = TextureFor(material, textures, aiTextureType_DIFFUSE_ROUGHNESS);
    if (!hasMetallicFactor && !out.metallicTexturePath.empty()) out.metallic = 1.0f;
    if (!hasRoughnessFactor && !out.roughnessTexturePath.empty()) out.roughness = 1.0f;
    out.heightTexturePath = TextureFor(material, textures, aiTextureType_DISPLACEMENT);
    if (out.heightTexturePath.empty()) out.heightTexturePath = TextureFor(material, textures, aiTextureType_HEIGHT);
    out.aoTexturePath = TextureFor(material, textures, aiTextureType_AMBIENT_OCCLUSION);
    out.emissiveTexturePath = TextureFor(material, textures, aiTextureType_EMISSIVE);
    if (out.emissiveTexturePath.empty()) out.emissiveTexturePath = TextureFor(material, textures, aiTextureType_EMISSION_COLOR);
    aiColor3D emissive;
    if (material.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == aiReturn_SUCCESS)
        out.emissive = {emissive.r, emissive.g, emissive.b, 1.0f};
    // Preserve every referenced texture, including maps the current PBR shader does not consume.
    if (textures.options.extractTextures)
        for (unsigned int type = 1; type <= AI_TEXTURE_TYPE_MAX; ++type)
            for (unsigned int index = 0; index < material.GetTextureCount(static_cast<aiTextureType>(type)); ++index)
            {
                aiString reference;
                if (material.GetTexture(static_cast<aiTextureType>(type), index, &reference) == aiReturn_SUCCESS)
                    textures.Get(reference.C_Str());
            }
    return out;
}

xm::Mat4 IdentityMatrix()
{
    return xm::Mat4Identity();
}

xm::Mat4 MatrixToRowMajor(const aiMatrix4x4& matrix)
{
    return xm::interop::FromAssimpRowMajor(matrix);
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
    std::unordered_map<std::string, xm::Mat4> inverseBindByName;
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

    // Animation-only FBX (e.g. a standalone Mixamo download) has no meshes and therefore no aiBone
    // entries, so the mesh scan above finds nothing. Fall back to treating every node targeted by an
    // animation channel as a bone: the node-hierarchy walk below then builds the skeleton from the
    // scene graph. Inverse-bind poses stay identity — they are only needed for skinning, which an
    // animation-only clip never does.
    if (boneNames.empty())
    {
        for (unsigned int animIndex = 0; animIndex < scene.mNumAnimations; ++animIndex)
        {
            const aiAnimation* animation = scene.mAnimations[animIndex];
            if (!animation)
                continue;
            for (unsigned int channelIndex = 0; channelIndex < animation->mNumChannels; ++channelIndex)
            {
                const aiNodeAnim* channel = animation->mChannels[channelIndex];
                if (channel)
                    boneNames[channel->mNodeName.C_Str()] = true;
            }
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
        // Mixamo exports always name their single take "mixamo.com" — meaningless and collision-
        // prone across separate walk/run downloads — so treat it (and an empty name) as a request
        // to fall back to the file stem, giving each clip a distinguishable name (e.g.
        // "Walking_anim_0").
        std::string takeName = aiAnim->mName.length > 0 ? std::string(aiAnim->mName.C_Str()) : std::string();
        std::string takeNameLower = takeName;
        std::transform(takeNameLower.begin(), takeNameLower.end(), takeNameLower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (takeName.empty() || takeNameLower == "mixamo.com")
            takeName = fbxPath.stem().string() + "_anim_" + std::to_string(animationIndex);
        animation.name = std::move(takeName);
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
            // A channel may key only some components (e.g. rotation alone): the others keep the
            // bone's rest value, never zero — a zero translation collapses the bone onto its parent.
            const AssimpImporter::AnimationKeyframe restKey = DecomposeTransform(
                xm::interop::ToAssimpRowMajor(skeleton.bones[static_cast<std::size_t>(boneIt->second)].localTransform), 0.0f);
            for (double tick : ticks)
            {
                AssimpImporter::AnimationKeyframe key = restKey;
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

ozz::math::Transform OzzTransformFromMatrix(const xm::Mat4& matrix)
{
    const aiMatrix4x4 aiMatrix = xm::interop::ToAssimpRowMajor(matrix);
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

// The runtime ozz skeleton for an import: exactly what writeOzzSidecars saves.
ozz::unique_ptr<ozz::animation::Skeleton> BuildOzzSkeleton(const AssimpImporter::SkeletonData& source,
                                                           std::string& error)
{
    ozz::animation::offline::RawSkeleton rawSkeleton;
    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(source.bones.size()); ++i)
    {
        if (source.bones[i].parentIndex < 0)
            roots.push_back(i);
    }
    rawSkeleton.roots.resize(roots.size());
    for (std::size_t i = 0; i < roots.size(); ++i)
        FillRawJoint(source, roots[i], rawSkeleton.roots[i]);

    if (!rawSkeleton.Validate())
    {
        error = "ozz RawSkeleton validation failed";
        return nullptr;
    }
    ozz::animation::offline::SkeletonBuilder skeletonBuilder;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton = skeletonBuilder(rawSkeleton);
    if (!skeleton)
        error = "ozz SkeletonBuilder failed";
    return skeleton;
}

bool NearlyEqual(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

// Same joints, in the same order, with the same rest pose (translations relative to their size,
// so centimetre and metre rigs compare alike; rotations up to sign).
bool SameSkeleton(const ozz::animation::Skeleton& a, const ozz::animation::Skeleton& b)
{
    if (a.num_joints() != b.num_joints())
        return false;
    const auto namesA = a.joint_names();
    const auto namesB = b.joint_names();
    const auto parentsA = a.joint_parents();
    const auto parentsB = b.joint_parents();
    for (int j = 0; j < a.num_joints(); ++j)
    {
        if (parentsA[j] != parentsB[j] || std::strcmp(namesA[j], namesB[j]) != 0)
            return false;
    }

    const auto lanes = [](const ozz::math::SimdFloat4& value) {
        std::array<float, 4> out{};
        ozz::math::StorePtrU(value, out.data());
        return out;
    };
    const auto restA = a.joint_rest_poses();
    const auto restB = b.joint_rest_poses();
    for (int soa = 0; soa < a.num_soa_joints(); ++soa)
    {
        const ozz::math::SoaTransform& ta = restA[soa];
        const ozz::math::SoaTransform& tb = restB[soa];
        const std::array<std::array<float, 4>, 3> transA{lanes(ta.translation.x), lanes(ta.translation.y), lanes(ta.translation.z)};
        const std::array<std::array<float, 4>, 3> transB{lanes(tb.translation.x), lanes(tb.translation.y), lanes(tb.translation.z)};
        const std::array<std::array<float, 4>, 3> scaleA{lanes(ta.scale.x), lanes(ta.scale.y), lanes(ta.scale.z)};
        const std::array<std::array<float, 4>, 3> scaleB{lanes(tb.scale.x), lanes(tb.scale.y), lanes(tb.scale.z)};
        const std::array<std::array<float, 4>, 4> rotA{lanes(ta.rotation.x), lanes(ta.rotation.y), lanes(ta.rotation.z), lanes(ta.rotation.w)};
        const std::array<std::array<float, 4>, 4> rotB{lanes(tb.rotation.x), lanes(tb.rotation.y), lanes(tb.rotation.z), lanes(tb.rotation.w)};
        for (int lane = 0; lane < 4 && soa * 4 + lane < a.num_joints(); ++lane)
        {
            float dot = 0.0f;
            for (int c = 0; c < 4; ++c)
                dot += rotA[c][lane] * rotB[c][lane];
            if (std::fabs(dot) < 0.9999f)
                return false;
            for (int c = 0; c < 3; ++c)
            {
                if (!NearlyEqual(transA[c][lane], transB[c][lane], 1e-3f) ||
                    !NearlyEqual(scaleA[c][lane], scaleB[c][lane], 1e-3f))
                    return false;
            }
        }
    }
    return true;
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
    // Collapse FBX pivot helper nodes so each bone is a single node whose name matches both its
    // animation channel target and its aiBone name. Left at Assimp's default, FBX rigs (Mixamo in
    // particular) emit intermediate "<bone>_$AssimpFbx$_Rotation/Translation" nodes that the
    // channel-to-bone lookup and the name-based retarget onto another rig cannot resolve. Clean
    // rigs without pivots are unaffected (no helper nodes are generated either way).
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);
    const aiScene* scene = importer.ReadFile(fbxPath.string(), kFbxImportFlags);
    // Assimp flags a scene AI_SCENE_FLAGS_INCOMPLETE when it carries no meshes — the normal state
    // for a standalone animation FBX (e.g. a Mixamo download). Tolerate that as long as the scene
    // still has a node graph and at least one animation to extract; only a genuinely unreadable
    // scene (null / no root) or an incomplete scene with nothing to salvage is fatal.
    const bool incompleteScene = scene && (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE);
    const bool sceneHasAnimations = scene && scene->mNumAnimations > 0;
    if (!scene || !scene->mRootNode || (incompleteScene && !sceneHasAnimations))
    {
        result.errorMessage = importer.GetErrorString();
        if (result.errorMessage.empty())
            result.errorMessage = "Assimp returned incomplete scene";
        return result;
    }

    const xm::Mat4 axisTransform = UpAxisTransform(*scene);
    result.skeletalIgnored = scene->HasAnimations();
    ExtractSkeleton(*scene, result);
    std::unordered_map<std::string, int> boneIndexByName;
    if (result.skeleton)
    {
        boneIndexByName = BuildBoneIndexMap(*result.skeleton);
        ExtractAnimations(*scene, fbxPath, *result.skeleton, result);
    }

    TextureImportContext textures{*scene, fbxPath, options, result};
    result.materials.reserve(std::max(1u, scene->mNumMaterials));
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
    {
        if (!scene->mMaterials[i])
            continue;
        result.materials.push_back(BuildMaterialSource(*scene->mMaterials[i], textures));
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
            ApplyPointTransform(axisTransform, vertex.position[0], vertex.position[1], vertex.position[2]);

            if (mesh->HasNormals())
            {
                vertex.normal[0] = mesh->mNormals[vertexIndex].x;
                vertex.normal[1] = mesh->mNormals[vertexIndex].y;
                vertex.normal[2] = mesh->mNormals[vertexIndex].z;
                ApplyVectorTransform(axisTransform, vertex.normal[0], vertex.normal[1], vertex.normal[2]);
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

    // A mesh-less FBX is still a valid import when it carries skeletal animation (a standalone
    // Mixamo-style animation file): the skeleton and animations were already extracted above and
    // are written as .ozz/.ixclip sidecars downstream. Only fail when there is neither mesh nor
    // animation data to import.
    const bool hasAnimationData = result.skeleton && !result.animations.empty();
    if (result.meshes.empty() && !hasAnimationData)
    {
        result.errorMessage = "no static mesh or animation data extracted";
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
                                      std::string& error,
                                      std::vector<std::string>* outJointNames) const
{
    if (!result.skeleton || result.skeleton->bones.empty())
    {
        error = "FBX does not contain a skeleton";
        return false;
    }

    ozz::unique_ptr<ozz::animation::Skeleton> skeleton = BuildOzzSkeleton(*result.skeleton, error);
    if (!skeleton)
        return false;
    if (!SaveOzzObject(skeletonPath, *skeleton, error))
        return false;

    // Built skeleton joint order (ozz reorders depth-first inside SkeletonBuilder, which can
    // differ from the raw bone order). The runtime Animation tracks MUST be in THIS order — not
    // raw bone order — for SamplingJob output to align with the skeleton joints. Map each raw
    // bone to its built joint index by name, and report joint_names (the clip's retarget key).
    const ozz::span<const char* const> builtJointNames = skeleton->joint_names();
    const int builtJointCount = static_cast<int>(builtJointNames.size());
    if (outJointNames)
    {
        outJointNames->clear();
        outJointNames->reserve(static_cast<std::size_t>(builtJointCount));
        for (const char* name : builtJointNames)
            outJointNames->emplace_back(name ? name : "");
    }
    std::vector<int> builtIndexForRaw(result.skeleton->bones.size(), -1);
    for (std::size_t r = 0; r < result.skeleton->bones.size(); ++r)
    {
        const std::string& rawName = result.skeleton->bones[r].name;
        for (int j = 0; j < builtJointCount; ++j)
        {
            if (builtJointNames[j] && rawName == builtJointNames[j])
            {
                builtIndexForRaw[r] = j;
                break;
            }
        }
    }

    std::size_t animationCount = std::min(animationPaths.size(), result.animations.size());
    for (std::size_t animIndex = 0; animIndex < animationCount; ++animIndex)
    {
        const AnimationData& source = result.animations[animIndex];
        if (source.duration <= 0.0f)
            continue;

        ozz::animation::offline::RawAnimation rawAnimation;
        rawAnimation.name = source.name.c_str();
        rawAnimation.duration = std::max(0.001f, source.duration);
        rawAnimation.tracks.resize(static_cast<std::size_t>(builtJointCount));
        for (const BoneTrack& track : source.tracks)
        {
            if (track.boneIndex < 0 || track.boneIndex >= static_cast<int>(builtIndexForRaw.size()))
                continue;
            const int builtIdx = builtIndexForRaw[track.boneIndex];
            if (builtIdx < 0 || builtIdx >= static_cast<int>(rawAnimation.tracks.size()))
                continue;
            auto& rawTrack = rawAnimation.tracks[builtIdx];
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
        // A joint the take never animates holds its rest pose: an empty ozz track samples as
        // identity, which would collapse the joint onto its parent.
        for (std::size_t r = 0; r < result.skeleton->bones.size(); ++r)
        {
            const int builtIdx = builtIndexForRaw[r];
            if (builtIdx < 0 || builtIdx >= static_cast<int>(rawAnimation.tracks.size()))
                continue;
            auto& rawTrack = rawAnimation.tracks[builtIdx];
            const ozz::math::Transform rest = OzzTransformFromMatrix(result.skeleton->bones[r].localTransform);
            if (rawTrack.translations.empty())
                rawTrack.translations.push_back({0.0f, rest.translation});
            if (rawTrack.rotations.empty())
                rawTrack.rotations.push_back({0.0f, rest.rotation});
            if (rawTrack.scales.empty())
                rawTrack.scales.push_back({0.0f, rest.scale});
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

bool AssimpImporter::ozzSkeletonSidecarMatches(const ImportResult& result,
                                               const std::filesystem::path& skeletonPath) const
{
    if (!result.skeleton || result.skeleton->bones.empty())
        return false;
    std::string error;
    const ozz::unique_ptr<ozz::animation::Skeleton> expected = BuildOzzSkeleton(*result.skeleton, error);
    if (!expected)
        return false;

    ozz::io::File file(skeletonPath.string().c_str(), "rb");
    if (!file.opened())
        return false;
    ozz::io::IArchive archive(&file);
    if (!archive.TestTag<ozz::animation::Skeleton>())
        return false;
    ozz::animation::Skeleton existing;
    archive >> existing;
    return SameSkeleton(*expected, existing);
}
