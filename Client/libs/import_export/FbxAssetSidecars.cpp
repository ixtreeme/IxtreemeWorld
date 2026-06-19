#include "FbxAssetSidecars.h"

#include "AssetDatabase.h"
#include "AssimpImporter.h"
#include "Debug.h"
#include "MaterialAssetManager.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
std::string SanitizeStem(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    while (!value.empty() && value.back() == '_')
        value.pop_back();
    return value.empty() ? "asset" : value;
}
}

bool ProcessImportedFbxAsset(const std::filesystem::path& destination,
                             const std::filesystem::path& libraryRoot,
                             std::string& error)
{
    AssimpImporter importer;
    AssimpImporter::ImportOptions options{};
    options.extractTextures = true;
    options.textureOutputDir = destination.parent_path() / (SanitizeStem(destination.stem().string()) + "_textures");
    Tracenf("[FBX-IMPORT] start path=%s target=%s",
        destination.generic_string().c_str(),
        destination.parent_path().generic_string().c_str());

    AssimpImporter::ImportResult result = importer.importFile(destination, options);
    if (!result.success)
    {
        error = result.errorMessage.empty() ? "FBX import failed" : result.errorMessage;
        TraceError("[FBX-IMPORT] failed path=%s error=%s",
            destination.generic_string().c_str(),
            error.c_str());
        return false;
    }

    AssetDatabase& db = AssetDatabase::Instance();
    std::vector<std::filesystem::path> animationPaths;
    std::optional<Guid> skeletonGuid;
    std::vector<Guid> animationGuids;
    if (result.hasSkeletal && result.skeleton)
    {
        const std::string stem = SanitizeStem(destination.stem().string());
        const std::filesystem::path skeletonPath = destination.parent_path() / (stem + "_skeleton.ozz");
        animationPaths.reserve(result.animations.size());
        for (std::size_t i = 0; i < result.animations.size(); ++i)
            animationPaths.push_back(destination.parent_path() / (stem + "_anim_" + std::to_string(i) + ".ozz"));
        std::string ozzError;
        if (!importer.writeOzzSidecars(result, skeletonPath, animationPaths, ozzError))
        {
            TraceError("[FBX-IMPORT] ozz sidecar generation failed path=%s error=%s",
                destination.generic_string().c_str(),
                ozzError.c_str());
        }
        else
        {
            db.runtimeAdd(skeletonPath);
            skeletonGuid = db.resolvePath(skeletonPath).value_or(db.getOrCreateGuid(skeletonPath));
            animationGuids.reserve(animationPaths.size());
            for (const std::filesystem::path& animationPath : animationPaths)
            {
                db.runtimeAdd(animationPath);
                animationGuids.push_back(db.resolvePath(animationPath).value_or(db.getOrCreateGuid(animationPath)));
            }
            Tracenf("[FBX-IMPORT] skeletal sidecars path=%s skeleton=%s animations=%zu",
                destination.generic_string().c_str(),
                skeletonPath.filename().generic_string().c_str(),
                animationPaths.size());
        }
    }
    if (std::filesystem::exists(options.textureOutputDir))
    {
        for (const auto& texture : std::filesystem::recursive_directory_iterator(options.textureOutputDir))
        {
            if (texture.is_regular_file())
                db.runtimeAdd(texture.path());
        }
    }

    std::vector<Guid> defaultMaterials;
    defaultMaterials.reserve(result.materials.size());
    MaterialAssetManager::ImportSummary summary{};
    summary.materials = static_cast<std::uint32_t>(result.materials.size());
    const std::filesystem::path materialsDir = libraryRoot / "materials" / SanitizeStem(destination.stem().string());
    for (std::size_t i = 0; i < result.materials.size(); ++i)
    {
        const GltfMaterialSource& material = result.materials[i];
        const std::string materialName = material.name.empty() ? ("material_" + std::to_string(i)) : material.name;
        defaultMaterials.push_back(MaterialAssetManager::Instance().createFromGltfMaterial(
            material,
            materialsDir,
            materialName,
            &summary));
    }

    for (const Guid& materialGuid : defaultMaterials)
    {
        if (const auto path = db.resolveGuid(materialGuid))
            db.runtimeAdd(*path);
    }
    db.runtimeAdd(destination);
    db.writeDefaultMaterials(destination, defaultMaterials);
    if (skeletonGuid)
        db.writeSkeletalAsset(destination, *skeletonGuid, animationGuids);

    Tracenf("[FBX-IMPORT] success path=%s meshes=%zu materials=%zu textures=%u bones=%zu animations=%zu",
        destination.generic_string().c_str(),
        result.meshes.size(),
        result.materials.size(),
        result.embeddedTextures + result.externalTextures,
        result.skeleton ? result.skeleton->bones.size() : 0u,
        result.animations.size());
    if (result.skeletalIgnored)
    {
        Tracenf("[FBX-IMPORT] skeletal data ignored for static import path=%s",
            destination.generic_string().c_str());
    }
    return true;
}
