#include "AssimpExporter.h"

#include "Debug.h"
#include "math/IXMath.h"

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/SceneCombiner.h>
#include <assimp/cexport.h>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kImportFlags =
    aiProcess_Triangulate |
    aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights |
    aiProcess_GenSmoothNormals |
    aiProcess_CalcTangentSpace |
    aiProcess_ValidateDataStructure;

aiMatrix4x4 ToAiMatrix(const float* values)
{
    return aiMatrix4x4(
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6], values[7],
        values[8], values[9], values[10], values[11],
        values[12], values[13], values[14], values[15]);
}

aiMatrix4x4 YUpToZUp()
{
    aiMatrix4x4 matrix;
    aiMatrix4x4::RotationX(ixtreeme::math::HalfPi, matrix);
    return matrix;
}

Assimp::ExportProperties BuildExportProperties(const AssimpExporter::ExportOptions& options)
{
    Assimp::ExportProperties properties;
#if defined(AI_CONFIG_EXPORT_FBX_EMBED_TEXTURES)
    properties.SetPropertyBool(AI_CONFIG_EXPORT_FBX_EMBED_TEXTURES, options.embedTextures);
#endif
    properties.SetPropertyBool("EXPORT_FBX_EMBED_TEXTURES", options.embedTextures);
    return properties;
}

bool ValidateSourcePath(const std::filesystem::path& sourcePath, std::string& error)
{
    std::error_code ec;
    if (sourcePath.empty() || !std::filesystem::exists(sourcePath, ec) || !std::filesystem::is_regular_file(sourcePath, ec))
    {
        error = "source model not found: " + sourcePath.generic_string();
        return false;
    }
    return true;
}

bool ValidateOutputPath(const std::filesystem::path& outputPath, std::string& error)
{
    if (outputPath.empty())
    {
        error = "output path is empty";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = outputPath.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            error = "failed to create output folder: " + ec.message();
            return false;
        }
    }
    return true;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
{
    bytes.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file.good();
}

std::string TextureFormatHint(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == "jpeg")
        ext = "jpg";
    if (ext.size() > HINTMAXTEXTURELEN - 1)
        ext.resize(HINTMAXTEXTURELEN - 1);
    return ext;
}

bool IsEmbeddedTextureReference(const std::string& texturePath)
{
    return !texturePath.empty() && texturePath.front() == '*';
}

std::filesystem::path ResolveTexturePath(const std::filesystem::path& sourceFolder, const std::string& texturePath)
{
    std::filesystem::path path(texturePath);
    if (path.is_absolute())
        return path;
    return sourceFolder / path;
}

std::string AppendEmbeddedTexture(aiScene* scene,
                                  const std::filesystem::path& texturePath,
                                  const std::vector<std::uint8_t>& bytes)
{
    const unsigned int textureIndex = scene->mNumTextures;
    aiTexture** textures = new aiTexture*[textureIndex + 1] {};
    for (unsigned int i = 0; i < scene->mNumTextures; ++i)
        textures[i] = scene->mTextures[i];
    delete[] scene->mTextures;
    scene->mTextures = textures;
    scene->mNumTextures = textureIndex + 1;

    aiTexture* texture = new aiTexture();
    texture->mWidth = static_cast<unsigned int>(bytes.size());
    texture->mHeight = 0;
    const std::size_t texelCount = (bytes.size() + sizeof(aiTexel) - 1u) / sizeof(aiTexel);
    texture->pcData = new aiTexel[texelCount];
    std::memset(texture->pcData, 0, texelCount * sizeof(aiTexel));
    std::memcpy(texture->pcData, bytes.data(), bytes.size());

    const std::string hint = TextureFormatHint(texturePath);
    std::memset(texture->achFormatHint, 0, sizeof(texture->achFormatHint));
    std::memcpy(texture->achFormatHint, hint.c_str(), std::min(hint.size(), sizeof(texture->achFormatHint) - 1u));
    texture->mFilename.Set(texturePath.filename().string());
    scene->mTextures[textureIndex] = texture;

    return "*" + std::to_string(textureIndex);
}

void ReplaceMaterialTexture(aiMaterial* material,
                            aiTextureType type,
                            unsigned int index,
                            const std::string& texturePath)
{
    aiString assimpPath(texturePath);
    material->RemoveProperty(AI_MATKEY_TEXTURE(type, index));
    material->AddProperty(&assimpPath, AI_MATKEY_TEXTURE(type, index));
}

void RemoveMaterialTexture(aiMaterial* material, aiTextureType type, unsigned int index)
{
    material->RemoveProperty(AI_MATKEY_TEXTURE(type, index));
}

void PrepareTexturesForFbxExport(aiScene* scene,
                                 const std::filesystem::path& sourcePath,
                                 const AssimpExporter::ExportOptions& options)
{
    if (!scene || scene->mNumMaterials == 0)
        return;

    const std::filesystem::path sourceFolder = sourcePath.parent_path();
    unsigned int embeddedCount = 0;
    unsigned int strippedCount = 0;

    for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
    {
        aiMaterial* material = scene->mMaterials[materialIndex];
        if (!material)
            continue;

        for (unsigned int textureTypeValue = aiTextureType_DIFFUSE;
             textureTypeValue < aiTextureType_UNKNOWN;
             ++textureTypeValue)
        {
            const aiTextureType textureType = static_cast<aiTextureType>(textureTypeValue);
            const unsigned int textureCount = material->GetTextureCount(textureType);
            std::vector<std::string> texturePaths;
            texturePaths.reserve(textureCount);
            for (unsigned int textureIndex = 0; textureIndex < textureCount; ++textureIndex)
            {
                aiString texturePath;
                if (material->GetTexture(textureType, textureIndex, &texturePath) == aiReturn_SUCCESS)
                    texturePaths.emplace_back(texturePath.C_Str());
                else
                    texturePaths.emplace_back();
            }

            for (unsigned int textureIndex = 0; textureIndex < texturePaths.size(); ++textureIndex)
            {
                const std::string& texturePath = texturePaths[textureIndex];
                if (texturePath.empty() || IsEmbeddedTextureReference(texturePath))
                    continue;

                const std::filesystem::path resolved = ResolveTexturePath(sourceFolder, texturePath);
                if (!options.embedTextures)
                {
                    RemoveMaterialTexture(material, textureType, textureIndex);
                    ++strippedCount;
                    continue;
                }

                std::vector<std::uint8_t> bytes;
                if (!ReadBinaryFile(resolved, bytes))
                {
                    RemoveMaterialTexture(material, textureType, textureIndex);
                    ++strippedCount;
                    TraceError("[FBX-EXPORT] texture dependency missing/failed source=%s texture=%s",
                        sourcePath.generic_string().c_str(),
                        texturePath.c_str());
                    continue;
                }

                ReplaceMaterialTexture(material, textureType, textureIndex, AppendEmbeddedTexture(scene, resolved, bytes));
                ++embeddedCount;
            }
        }
    }

    if (embeddedCount > 0 || strippedCount > 0)
    {
        Tracenf("[FBX-EXPORT] prepared textures source=%s embedded=%u stripped=%u",
            sourcePath.generic_string().c_str(),
            embeddedCount,
            strippedCount);
    }
}

} // namespace

bool AssimpExporter::ExportAssetToFbx(const std::filesystem::path& sourcePath,
                                      const ExportOptions& options,
                                      std::string& error)
{
    SourceAsset source{};
    source.path = sourcePath;
    source.nodeName = sourcePath.stem().string();
    return ExportAssetsToFbx({source}, options, error);
}

bool AssimpExporter::ExportAssetsToFbx(const std::vector<SourceAsset>& sources,
                                       const ExportOptions& options,
                                       std::string& error)
{
    error.clear();
    if (sources.empty())
    {
        error = "no mesh sources selected";
        return false;
    }

    if (!ValidateOutputPath(options.outputPath, error))
        return false;

    std::vector<aiScene*> sceneCopies;
    sceneCopies.reserve(sources.size());
    unsigned int totalMeshes = 0;
    unsigned int totalMaterials = 0;
    unsigned int totalAnimations = 0;

    for (const SourceAsset& source : sources)
    {
        if (!ValidateSourcePath(source.path, error))
        {
            for (aiScene* scene : sceneCopies)
                aiFreeScene(scene);
            return false;
        }

        Assimp::Importer importer;
        const aiScene* imported = importer.ReadFile(source.path.string(), kImportFlags);
        if (!imported)
        {
            error = importer.GetErrorString();
            TraceError("[FBX-EXPORT] import failed source=%s error=%s",
                source.path.generic_string().c_str(),
                error.c_str());
            for (aiScene* scene : sceneCopies)
                aiFreeScene(scene);
            return false;
        }

        aiScene* copy = nullptr;
        aiCopyScene(imported, &copy);
        if (!copy)
        {
            error = "failed to copy imported scene";
            for (aiScene* scene : sceneCopies)
                aiFreeScene(scene);
            return false;
        }
        if (copy->mRootNode)
        {
            if (!source.nodeName.empty())
                copy->mRootNode->mName.Set(source.nodeName);
            copy->mRootNode->mTransformation = ToAiMatrix(source.transform) * copy->mRootNode->mTransformation;
        }
        PrepareTexturesForFbxExport(copy, source.path, options);
        totalMeshes += copy->mNumMeshes;
        totalMaterials += copy->mNumMaterials;
        totalAnimations += copy->mNumAnimations;
        sceneCopies.push_back(copy);
    }

    aiScene* exportScene = nullptr;
    if (sceneCopies.size() == 1)
    {
        exportScene = sceneCopies.front();
        sceneCopies.clear();
    }
    else
    {
        Assimp::SceneCombiner::MergeScenes(&exportScene, sceneCopies);
    }

    if (!exportScene)
    {
        error = "failed to build export scene";
        return false;
    }
    if (exportScene->mRootNode)
        exportScene->mRootNode->mTransformation = YUpToZUp() * exportScene->mRootNode->mTransformation;

    Assimp::Exporter exporter;
    Assimp::ExportProperties properties = BuildExportProperties(options);
    aiReturn result = aiReturn_FAILURE;
    try
    {
        result = exporter.Export(exportScene, options.format, options.outputPath.string(), 0u, &properties);
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        TraceError("[FBX-EXPORT] failed path=%s exception=%s",
            options.outputPath.generic_string().c_str(),
            error.c_str());
        aiFreeScene(exportScene);
        return false;
    }
    if (result != aiReturn_SUCCESS)
    {
        error = exporter.GetErrorString();
        TraceError("[FBX-EXPORT] failed path=%s error=%s",
            options.outputPath.generic_string().c_str(),
            error.c_str());
        aiFreeScene(exportScene);
        return false;
    }

    Tracenf("[FBX-EXPORT] success path=%s sources=%zu meshes=%u materials=%u animations=%u embedTextures=%s",
        options.outputPath.generic_string().c_str(),
        sources.size(),
        totalMeshes,
        totalMaterials,
        options.exportAnimations ? totalAnimations : 0u,
        options.embedTextures ? "yes" : "no");
    aiFreeScene(exportScene);
    return true;
}
