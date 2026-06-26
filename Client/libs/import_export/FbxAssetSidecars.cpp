#include "FbxAssetSidecars.h"

#include "AssetDatabase.h"
#include "AssimpImporter.h"
#include "Debug.h"
#include "MaterialAssetManager.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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

std::string EscapeJsonInline(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value)
    {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Writes a retargetable .ixclip wrapper next to the model's ozz sidecars (under the library's
// animation_clips/ folder). joint_names are the SOURCE bones in track order (track i targets
// bones[i]) — the retarget key. Keys must match AssetLibrary's ReadAnimationClipJson.
bool WriteAnimationClipSidecar(const std::filesystem::path& clipPath,
                               const std::string& id,
                               const std::string& displayName,
                               const std::string& sourceAnimGuid,
                               const std::string& sourceSkeletonGuid,
                               const std::string& sourceAnimPath,
                               float duration,
                               const std::vector<std::string>& jointNames)
{
    std::ostringstream json;
    json << "{\n"
         << "  \"version\": 1,\n"
         << "  \"id\": \"" << EscapeJsonInline(id) << "\",\n"
         << "  \"display_name\": \"" << EscapeJsonInline(displayName) << "\",\n"
         << "  \"source_anim_guid\": \"" << EscapeJsonInline(sourceAnimGuid) << "\",\n"
         << "  \"source_skeleton_guid\": \"" << EscapeJsonInline(sourceSkeletonGuid) << "\",\n"
         << "  \"source_anim_path\": \"" << EscapeJsonInline(sourceAnimPath) << "\",\n"
         << "  \"duration\": " << duration << ",\n"
         << "  \"loop\": 1,\n"
         << "  \"sample_rate\": 30.0,\n"
         << "  \"root_joint\": \"" << (jointNames.empty() ? std::string() : EscapeJsonInline(jointNames.front())) << "\",\n"
         << "  \"root_motion_mode\": \"none\",\n"
         << "  \"joint_names\": [";
    for (std::size_t j = 0; j < jointNames.size(); ++j)
    {
        if (j != 0)
            json << ", ";
        json << "\"" << EscapeJsonInline(jointNames[j]) << "\"";
    }
    json << "]\n}\n";

    std::error_code ec;
    std::filesystem::create_directories(clipPath.parent_path(), ec);
    std::ofstream out(clipPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    const std::string text = json.str();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
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
        std::vector<std::string> builtJointNames;
        if (!importer.writeOzzSidecars(result, skeletonPath, animationPaths, ozzError, &builtJointNames))
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

            // Emit a retargetable .ixclip wrapper per animation into the library's
            // animation_clips/ folder so each becomes a standalone, skeleton-agnostic clip
            // (AssetLibrary::ReconcileFilesystem discovers them as browser entries).
            const std::filesystem::path clipDir = libraryRoot / "animation_clips" / stem;
            const std::string skeletonGuidStr = skeletonGuid ? skeletonGuid->toString() : std::string();
            for (std::size_t i = 0; i < animationPaths.size() && i < result.animations.size(); ++i)
            {
                if (!std::filesystem::exists(animationPaths[i]))
                    continue;  // animation was skipped during ozz generation (e.g. zero duration)
                const std::string clipName = result.animations[i].name.empty()
                    ? (stem + "_anim_" + std::to_string(i))
                    : result.animations[i].name;
                const std::string clipId = "clip_" + SanitizeStem(stem + "_" + clipName);
                const std::filesystem::path clipPath = clipDir / (SanitizeStem(clipName) + ".ixclip");
                const std::string animGuidStr = (i < animationGuids.size()) ? animationGuids[i].toString() : std::string();
                if (WriteAnimationClipSidecar(clipPath, clipId, clipName, animGuidStr, skeletonGuidStr,
                        animationPaths[i].generic_string(), result.animations[i].duration, builtJointNames))
                {
                    db.runtimeAdd(clipPath);
                    Tracenf("[FBX-IMPORT] animation clip emitted name=%s joints=%zu",
                        clipName.c_str(),
                        builtJointNames.size());
                }
            }
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
