#include "GrannyModel.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <granny.h>

#include <cstdarg>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
void Log(const char* text)
{
    Tracen(text);
}

void LogFormat(const char* format, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

granny_file* ReadGrannyAsset(client::asset::IAssetReader& assets, const std::string& path,
    const char* logTag)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        LogFormat("[%s] failed to read asset: %s", logTag, path.c_str());
        return nullptr;
    }

    // Granny copies the parsed file into its own allocation; the asset buffer only has to
    // stay alive during this call.
    return GrannyReadEntireFileFromMemory(static_cast<granny_int32x>(bytes->size()), bytes->data());
}

const char* Safe(const char* text)
{
    return text ? text : "<null>";
}

const char* MemberTypeName(granny_member_type type)
{
    switch (type)
    {
    case GrannyEndMember: return "End";
    case GrannyInlineMember: return "Inline";
    case GrannyReferenceMember: return "Reference";
    case GrannyReferenceToArrayMember: return "ReferenceToArray";
    case GrannyArrayOfReferencesMember: return "ArrayOfReferences";
    case GrannyVariantReferenceMember: return "VariantReference";
    case GrannyReferenceToVariantArrayMember: return "ReferenceToVariantArray";
    case GrannyStringMember: return "String";
    case GrannyTransformMember: return "Transform";
    case GrannyReal32Member: return "Real32";
    case GrannyInt8Member: return "Int8";
    case GrannyUInt8Member: return "UInt8";
    case GrannyBinormalInt8Member: return "BinormalInt8";
    case GrannyNormalUInt8Member: return "NormalUInt8";
    case GrannyInt16Member: return "Int16";
    case GrannyUInt16Member: return "UInt16";
    case GrannyBinormalInt16Member: return "BinormalInt16";
    case GrannyNormalUInt16Member: return "NormalUInt16";
    case GrannyInt32Member: return "Int32";
    case GrannyUInt32Member: return "UInt32";
    case GrannyReal16Member: return "Real16";
    case GrannyEmptyReferenceMember: return "EmptyReference";
    default: return "Unknown";
    }
}

int CountTypeMembers(granny_data_type_definition* type)
{
    if (!type)
        return 0;

    int count = 0;
    while (type[count].Type != GrannyEndMember)
        ++count;
    return count;
}

bool NameContains(const char* name, const char* token)
{
    return name && std::strstr(name, token) != nullptr;
}

void LogVertexLayout(granny_data_type_definition* type)
{
    if (!type)
    {
        Log("[GRANNY] vertex layout: <null>");
        return;
    }

    const int memberCount = CountTypeMembers(type);

    bool hasPosition = false;
    bool hasNormal = false;
    bool hasUV = false;
    bool hasWeights = false;
    bool hasBoneIndices = false;
    int vertexBytes = 0;

    for (int i = 0; i < memberCount; ++i)
    {
        const granny_data_type_definition& member = type[i];
        const int memberSize = GrannyGetMemberTypeSize(&member);
        vertexBytes += memberSize;
        LogFormat("[GRANNY]   member[%d] name='%s' type=%s arrayWidth=%d size=%d",
            i,
            Safe(member.Name),
            MemberTypeName(member.Type),
            member.ArrayWidth,
            memberSize);

        hasPosition = hasPosition || NameContains(member.Name, "Position");
        hasNormal = hasNormal || NameContains(member.Name, "Normal");
        hasUV = hasUV || NameContains(member.Name, "Texture") || NameContains(member.Name, "UV");
        hasWeights = hasWeights || NameContains(member.Name, "Weights") || NameContains(member.Name, "BoneWeights");
        hasBoneIndices = hasBoneIndices || NameContains(member.Name, "BoneIndices");
    }

    LogFormat("[GRANNY] vertex layout members=%d vertexBytes=%d definitionBytes=%d boneCount=%d channelCount=%d componentCount=%d",
        memberCount,
        vertexBytes,
        GrannyGetTotalTypeSize(type),
        GrannyGetVertexBoneCount(type),
        GrannyGetVertexChannelCount(type),
        GrannyGetVertexComponentCount(type));

    LogFormat("[GRANNY] vertex layout summary: Position=%d Normal=%d UV=%d BoneWeights=%d BoneIndices=%d",
        hasPosition ? 1 : 0,
        hasNormal ? 1 : 0,
        hasUV ? 1 : 0,
        hasWeights ? 1 : 0,
        hasBoneIndices ? 1 : 0);
}

void LogMaterial(granny_material* material, int index)
{
    if (!material)
    {
        LogFormat("[GRANNY] material[%d]: <null>", index);
        return;
    }

    const granny_texture* texture = material->Texture;
    LogFormat("[GRANNY] material[%d] name='%s' mapCount=%d texture='%s'",
        index,
        Safe(material->Name),
        material->MapCount,
        texture ? Safe(texture->FromFileName) : "<none>");

    for (int mapIndex = 0; mapIndex < material->MapCount; ++mapIndex)
    {
        const granny_material_map& map = material->Maps[mapIndex];
        LogFormat("[GRANNY]   map[%d] usage='%s' material='%s' texture='%s'",
            mapIndex,
            Safe(map.Usage),
            map.Material ? Safe(map.Material->Name) : "<null>",
            (map.Material && map.Material->Texture) ? Safe(map.Material->Texture->FromFileName) : "<none>");
    }
}

void LogMesh(granny_mesh* mesh, int index)
{
    if (!mesh)
    {
        LogFormat("[GRANNY] mesh[%d]: <null>", index);
        return;
    }

    const int vertexCount = GrannyGetMeshVertexCount(mesh);
    const int indexCount = GrannyGetMeshIndexCount(mesh);
    const int triGroupCount = GrannyGetMeshTriangleGroupCount(mesh);
    const int triangleCount = GrannyGetMeshTriangleCount(mesh);
    const int bytesPerIndex = GrannyGetMeshBytesPerIndex(mesh);

    LogFormat("[GRANNY] mesh[%d] '%s': verts=%d indices=%d triangles=%d triGroups=%d bytesPerIndex=%d materials=%d bones=%d rigid=%d",
        index,
        Safe(mesh->Name),
        vertexCount,
        indexCount,
        triangleCount,
        triGroupCount,
        bytesPerIndex,
        mesh->MaterialBindingCount,
        mesh->BoneBindingCount,
        GrannyMeshIsRigid(mesh) ? 1 : 0);

    LogVertexLayout(GrannyGetMeshVertexType(mesh));

    granny_tri_material_group* groups = GrannyGetMeshTriangleGroups(mesh);
    for (int groupIndex = 0; groupIndex < triGroupCount; ++groupIndex)
    {
        const granny_tri_material_group& group = groups[groupIndex];
        granny_material* material = nullptr;
        if (group.MaterialIndex >= 0 && group.MaterialIndex < mesh->MaterialBindingCount)
            material = mesh->MaterialBindings[group.MaterialIndex].Material;

        const char* textureName = (material && material->Texture) ? material->Texture->FromFileName : "<none>";
        LogFormat("[GRANNY]   triGroup[%d] materialIndex=%d triFirst=%d triCount=%d material='%s' texture='%s'",
            groupIndex,
            group.MaterialIndex,
            group.TriFirst,
            group.TriCount,
            material ? Safe(material->Name) : "<null>",
            Safe(textureName));
    }

    for (int materialIndex = 0; materialIndex < mesh->MaterialBindingCount; ++materialIndex)
        LogMaterial(mesh->MaterialBindings[materialIndex].Material, materialIndex);
}

std::vector<std::string> CollectFirstSkeletonBones(granny_file_info* info)
{
    std::vector<std::string> bones;
    if (!info || info->SkeletonCount <= 0 || !info->Skeletons[0])
        return bones;

    const granny_skeleton* skeleton = info->Skeletons[0];
    bones.reserve(static_cast<size_t>(skeleton->BoneCount));
    for (int boneIndex = 0; boneIndex < skeleton->BoneCount; ++boneIndex)
        bones.emplace_back(Safe(skeleton->Bones[boneIndex].Name));
    return bones;
}

void LogAnimArtToolInfo(granny_file_info* info)
{
    if (!info || !info->ArtToolInfo)
    {
        Log("[ANIM] artTool=<none>");
        return;
    }

    const granny_art_tool_info* art = info->ArtToolInfo;
    LogFormat("[ANIM] artTool='%s' rev=%d.%d pointerSize=%d unitsPerMeter=%.6f",
        Safe(art->FromArtToolName),
        art->ArtToolMajorRevision,
        art->ArtToolMinorRevision,
        art->ArtToolPointerSize,
        art->UnitsPerMeter);
    LogFormat("[ANIM] basis origin=(%.6f, %.6f, %.6f) right=(%.6f, %.6f, %.6f) up=(%.6f, %.6f, %.6f) back=(%.6f, %.6f, %.6f)",
        art->Origin[0], art->Origin[1], art->Origin[2],
        art->RightVector[0], art->RightVector[1], art->RightVector[2],
        art->UpVector[0], art->UpVector[1], art->UpVector[2],
        art->BackVector[0], art->BackVector[1], art->BackVector[2]);
}

void LogNameListPrefix(const char* prefix, const std::vector<std::string>& names, size_t maxCount)
{
    std::string line(prefix);
    const size_t count = std::min(names.size(), maxCount);
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            line += ", ";
        line += names[i];
    }
    if (names.size() > count)
        line += ", ...";
    Log(line.c_str());
}

std::unordered_set<std::string> BuildSkeletonBoneSet(const granny_skeleton* skeleton)
{
    std::unordered_set<std::string> bones;
    if (!skeleton)
        return bones;

    for (int boneIndex = 0; boneIndex < skeleton->BoneCount; ++boneIndex)
        bones.insert(Safe(skeleton->Bones[boneIndex].Name));
    return bones;
}

int CountMatchingAnimationTracks(const granny_animation* animation, const std::unordered_set<std::string>& modelBoneSet, int* outTrackCount)
{
    int matchCount = 0;
    int trackCount = 0;
    if (!animation)
    {
        if (outTrackCount)
            *outTrackCount = 0;
        return 0;
    }

    for (int groupIndex = 0; groupIndex < animation->TrackGroupCount; ++groupIndex)
    {
        granny_track_group* group = animation->TrackGroups[groupIndex];
        if (!group)
            continue;

        for (int trackIndex = 0; trackIndex < group->TransformTrackCount; ++trackIndex)
        {
            ++trackCount;
            const char* name = Safe(group->TransformTracks[trackIndex].Name);
            if (modelBoneSet.find(name) != modelBoneSet.end())
                ++matchCount;
        }
    }

    if (outTrackCount)
        *outTrackCount = trackCount;
    return matchCount;
}

bool MatrixIsFinite(const granny_real32* matrix)
{
    if (!matrix)
        return false;

    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(matrix[i]))
            return false;
    }
    return true;
}

bool MatrixHasNonZeroElement(const granny_real32* matrix)
{
    if (!matrix)
        return false;

    for (int i = 0; i < 16; ++i)
    {
        if (std::fabs(matrix[i]) > 0.000001f)
            return true;
    }
    return false;
}

void LogPoseMatrix(const char* label, int index, const char* boneName, const granny_real32* matrix)
{
    if (!matrix)
    {
        LogFormat("[POSE] %s[%d] '%s': <null>", label, index, Safe(boneName));
        return;
    }

    LogFormat("[POSE] %s[%d] '%s': row0=(%.6f, %.6f, %.6f, %.6f) row1=(%.6f, %.6f, %.6f, %.6f) row2=(%.6f, %.6f, %.6f, %.6f) row3=(%.6f, %.6f, %.6f, %.6f) translation=(%.6f, %.6f, %.6f) finite=%d nonZero=%d",
        label,
        index,
        Safe(boneName),
        matrix[0], matrix[1], matrix[2], matrix[3],
        matrix[4], matrix[5], matrix[6], matrix[7],
        matrix[8], matrix[9], matrix[10], matrix[11],
        matrix[12], matrix[13], matrix[14], matrix[15],
        matrix[12], matrix[13], matrix[14],
        MatrixIsFinite(matrix) ? 1 : 0,
        MatrixHasNonZeroElement(matrix) ? 1 : 0);
}
}

struct GrannyModel::Impl
{
    granny_file* file = nullptr;
};

GrannyModel::GrannyModel(client::asset::IAssetReader& assets)
    : m_assets(&assets)
{
}

GrannyModel::~GrannyModel()
{
    Destroy();
}

bool GrannyModel::LoadAndLog(const std::string& path)
{
    Destroy();
    m_impl = new Impl();

    LogFormat("[GRANNY] loading: %s", path.c_str());
    m_impl->file = m_assets ? ReadGrannyAsset(*m_assets, path, "GRANNY") : nullptr;
    if (!m_impl->file)
    {
        Log("[GRANNY] GrannyReadEntireFileFromMemory failed");
        Destroy();
        return false;
    }

    LogFormat("[GRANNY] file loaded: %s", path.c_str());
    granny_file_info* info = GrannyGetFileInfo(m_impl->file);
    if (!info)
    {
        Log("[GRANNY] GrannyGetFileInfo failed");
        Destroy();
        return false;
    }

    if (info->ArtToolInfo)
    {
        const granny_art_tool_info* art = info->ArtToolInfo;
        LogFormat("[GRANNY] artTool='%s' rev=%d.%d pointerSize=%d unitsPerMeter=%.6f",
            Safe(art->FromArtToolName),
            art->ArtToolMajorRevision,
            art->ArtToolMinorRevision,
            art->ArtToolPointerSize,
            art->UnitsPerMeter);
        LogFormat("[GRANNY] basis origin=(%.6f, %.6f, %.6f) right=(%.6f, %.6f, %.6f) up=(%.6f, %.6f, %.6f) back=(%.6f, %.6f, %.6f)",
            art->Origin[0], art->Origin[1], art->Origin[2],
            art->RightVector[0], art->RightVector[1], art->RightVector[2],
            art->UpVector[0], art->UpVector[1], art->UpVector[2],
            art->BackVector[0], art->BackVector[1], art->BackVector[2]);
    }

    if (info->ExporterInfo)
    {
        const granny_exporter_info* exporter = info->ExporterInfo;
        LogFormat("[GRANNY] exporter='%s' rev=%d.%d customization=%d build=%d",
            Safe(exporter->ExporterName),
            exporter->ExporterMajorRevision,
            exporter->ExporterMinorRevision,
            exporter->ExporterCustomization,
            exporter->ExporterBuildNumber);
    }

    int boneCount = 0;
    for (int i = 0; i < info->SkeletonCount; ++i)
        if (info->Skeletons[i])
            boneCount += info->Skeletons[i]->BoneCount;

    LogFormat("[GRANNY] models=%d, meshes=%d, textures=%d, materials=%d, skeletons=%d, bones=%d, vertexDatas=%d, triTopologies=%d, animations=%d",
        info->ModelCount,
        info->MeshCount,
        info->TextureCount,
        info->MaterialCount,
        info->SkeletonCount,
        boneCount,
        info->VertexDataCount,
        info->TriTopologyCount,
        info->AnimationCount);

    for (int textureIndex = 0; textureIndex < info->TextureCount; ++textureIndex)
    {
        const granny_texture* texture = info->Textures[textureIndex];
        LogFormat("[GRANNY] texture[%d]: file='%s' type=%d encoding=%d size=%dx%d images=%d",
            textureIndex,
            texture ? Safe(texture->FromFileName) : "<null>",
            texture ? texture->TextureType : -1,
            texture ? texture->Encoding : -1,
            texture ? texture->Width : 0,
            texture ? texture->Height : 0,
            texture ? texture->ImageCount : 0);
    }

    for (int skeletonIndex = 0; skeletonIndex < info->SkeletonCount; ++skeletonIndex)
    {
        const granny_skeleton* skeleton = info->Skeletons[skeletonIndex];
        LogFormat("[GRANNY] skeleton[%d] name='%s' bones=%d lodType=%d",
            skeletonIndex,
            skeleton ? Safe(skeleton->Name) : "<null>",
            skeleton ? skeleton->BoneCount : 0,
            skeleton ? skeleton->LODType : 0);

        if (skeleton)
        {
            for (int boneIndex = 0; boneIndex < skeleton->BoneCount; ++boneIndex)
            {
                const granny_bone& bone = skeleton->Bones[boneIndex];
                LogFormat("[GRANNY]   bone[%d] name='%s' parent=%d lodError=%.6f",
                    boneIndex,
                    Safe(bone.Name),
                    bone.ParentIndex,
                    bone.LODError);
            }
        }
    }

    for (int modelIndex = 0; modelIndex < info->ModelCount; ++modelIndex)
    {
        const granny_model* model = info->Models[modelIndex];
        LogFormat("[GRANNY] model[%d] name='%s' meshBindings=%d skeleton='%s'",
            modelIndex,
            model ? Safe(model->Name) : "<null>",
            model ? model->MeshBindingCount : 0,
            (model && model->Skeleton) ? Safe(model->Skeleton->Name) : "<none>");

        if (model)
        {
            for (int bindingIndex = 0; bindingIndex < model->MeshBindingCount; ++bindingIndex)
            {
                granny_mesh* mesh = model->MeshBindings[bindingIndex].Mesh;
                LogFormat("[GRANNY]   meshBinding[%d] mesh='%s'",
                    bindingIndex,
                    mesh ? Safe(mesh->Name) : "<null>");
            }
        }
    }

    for (int meshIndex = 0; meshIndex < info->MeshCount; ++meshIndex)
        LogMesh(info->Meshes[meshIndex], meshIndex);

    return true;
}

bool GrannyModel::LoadAnimationAndCompare(const std::string& modelPath, const std::string& animationPath)
{
    LogFormat("[ANIM] loading model for bone comparison: %s", modelPath.c_str());
    granny_file* modelFile = m_assets ? ReadGrannyAsset(*m_assets, modelPath, "ANIM") : nullptr;
    if (!modelFile)
    {
        Log("[ANIM] model GrannyReadEntireFileFromMemory failed");
        return false;
    }

    granny_file_info* modelInfo = GrannyGetFileInfo(modelFile);
    if (!modelInfo)
    {
        Log("[ANIM] model GrannyGetFileInfo failed");
        GrannyFreeFile(modelFile);
        return false;
    }

    std::vector<std::string> modelBones = CollectFirstSkeletonBones(modelInfo);
    const char* modelSkeletonName = (modelInfo->SkeletonCount > 0 && modelInfo->Skeletons[0]) ?
        Safe(modelInfo->Skeletons[0]->Name) : "<none>";
    LogFormat("[ANIM] model skeleton '%s' bones=%zu", modelSkeletonName, modelBones.size());
    LogNameListPrefix("[ANIM] model skeleton first bones: ", modelBones, 10);

    std::unordered_set<std::string> modelBoneSet;
    for (const std::string& bone : modelBones)
        modelBoneSet.insert(bone);

    LogFormat("[ANIM] loading: %s", animationPath.c_str());
    granny_file* animFile = m_assets ? ReadGrannyAsset(*m_assets, animationPath, "ANIM") : nullptr;
    if (!animFile)
    {
        Log("[ANIM] selected.gr2 GrannyReadEntireFileFromMemory failed");
        GrannyFreeFile(modelFile);
        return false;
    }

    granny_file_info* animInfo = GrannyGetFileInfo(animFile);
    if (!animInfo)
    {
        Log("[ANIM] selected.gr2 GrannyGetFileInfo failed");
        GrannyFreeFile(animFile);
        GrannyFreeFile(modelFile);
        return false;
    }

    Log("[ANIM] selected.gr2 loaded");
    LogAnimArtToolInfo(animInfo);

    int animBoneCount = 0;
    for (int i = 0; i < animInfo->SkeletonCount; ++i)
        if (animInfo->Skeletons[i])
            animBoneCount += animInfo->Skeletons[i]->BoneCount;

    LogFormat("[ANIM] animations=%d, meshes=%d, models=%d, skeletons=%d, skeletonBones=%d, trackGroups(file)=%d",
        animInfo->AnimationCount,
        animInfo->MeshCount,
        animInfo->ModelCount,
        animInfo->SkeletonCount,
        animBoneCount,
        animInfo->TrackGroupCount);

    std::vector<std::string> firstTrackNames;
    int firstTrackMatchCount = 0;
    int firstTrackCount = 0;

    for (int animIndex = 0; animIndex < animInfo->AnimationCount; ++animIndex)
    {
        granny_animation* animation = animInfo->Animations[animIndex];
        if (!animation)
        {
            LogFormat("[ANIM] anim[%d]: <null>", animIndex);
            continue;
        }

        int totalTransformTracks = 0;
        for (int groupIndex = 0; groupIndex < animation->TrackGroupCount; ++groupIndex)
        {
            granny_track_group* group = animation->TrackGroups[groupIndex];
            if (group)
                totalTransformTracks += group->TransformTrackCount;
        }

        const float sampleRate = animation->TimeStep > 0.0f ? (1.0f / animation->TimeStep) : 0.0f;
        LogFormat("[ANIM] anim[%d] '%s': duration=%.6fs timeStep=%.6f sampleRate=%.3f trackGroups=%d transformTracks=%d oversampling=%.3f loops=%d flags=0x%08x",
            animIndex,
            Safe(animation->Name),
            animation->Duration,
            animation->TimeStep,
            sampleRate,
            animation->TrackGroupCount,
            totalTransformTracks,
            animation->Oversampling,
            animation->DefaultLoopCount,
            animation->Flags);

        for (int groupIndex = 0; groupIndex < animation->TrackGroupCount; ++groupIndex)
        {
            granny_track_group* group = animation->TrackGroups[groupIndex];
            if (!group)
            {
                LogFormat("[ANIM]   trackGroup[%d]: <null>", groupIndex);
                continue;
            }

            LogFormat("[ANIM]   trackGroup[%d] '%s': transformTracks=%d vectorTracks=%d textTracks=%d flags=0x%08x",
                groupIndex,
                Safe(group->Name),
                group->TransformTrackCount,
                group->VectorTrackCount,
                group->TextTrackCount,
                group->Flags);

            if (animIndex == 0 && groupIndex == 0)
            {
                firstTrackCount = group->TransformTrackCount;
                firstTrackNames.reserve(static_cast<size_t>(group->TransformTrackCount));
                for (int trackIndex = 0; trackIndex < group->TransformTrackCount; ++trackIndex)
                {
                    const granny_transform_track& track = group->TransformTracks[trackIndex];
                    const std::string trackName = Safe(track.Name);
                    firstTrackNames.push_back(trackName);
                    if (modelBoneSet.find(trackName) != modelBoneSet.end())
                        ++firstTrackMatchCount;
                }
            }
        }
    }

    LogNameListPrefix("[ANIM] anim track names first 10: ", firstTrackNames, 10);
    for (size_t i = 0; i < firstTrackNames.size(); ++i)
        LogFormat("[ANIM]   track[%zu] name='%s' modelBoneMatch=%d",
            i,
            firstTrackNames[i].c_str(),
            modelBoneSet.find(firstTrackNames[i]) != modelBoneSet.end() ? 1 : 0);

    LogFormat("[ANIM] bone match: %d of %d animation tracks match model bones",
        firstTrackMatchCount,
        firstTrackCount);

    GrannyFreeFile(animFile);
    GrannyFreeFile(modelFile);
    return true;
}

bool GrannyModel::ComputeStaticPoseAndLog(const std::string& modelPath, const std::string& animationPath, float timeSeconds)
{
    LogFormat("[POSE] loading model='%s' animation='%s' t=%.6f",
        modelPath.c_str(),
        animationPath.c_str(),
        timeSeconds);

    granny_file* modelFile = m_assets ? ReadGrannyAsset(*m_assets, modelPath, "POSE") : nullptr;
    if (!modelFile)
    {
        Log("[POSE] model GrannyReadEntireFileFromMemory failed");
        return false;
    }

    granny_file* animFile = nullptr;
    granny_model_instance* modelInstance = nullptr;
    granny_control* control = nullptr;
    granny_local_pose* localPose = nullptr;
    granny_world_pose* worldPose = nullptr;

    auto cleanup = [&]()
    {
        if (worldPose)
            GrannyFreeWorldPose(worldPose);
        if (localPose)
            GrannyFreeLocalPose(localPose);
        if (control)
            GrannyFreeControl(control);
        if (modelInstance)
            GrannyFreeModelInstance(modelInstance);
        if (animFile)
            GrannyFreeFile(animFile);
        if (modelFile)
            GrannyFreeFile(modelFile);
    };

    granny_file_info* modelInfo = GrannyGetFileInfo(modelFile);
    if (!modelInfo || modelInfo->ModelCount <= 0 || !modelInfo->Models[0])
    {
        Log("[POSE] model file has no usable model");
        cleanup();
        return false;
    }

    granny_model* model = modelInfo->Models[0];
    granny_skeleton* skeleton = model->Skeleton;
    if (!skeleton || skeleton->BoneCount <= 0)
    {
        Log("[POSE] model has no usable skeleton");
        cleanup();
        return false;
    }

    animFile = m_assets ? ReadGrannyAsset(*m_assets, animationPath, "POSE") : nullptr;
    if (!animFile)
    {
        Log("[POSE] animation GrannyReadEntireFileFromMemory failed");
        cleanup();
        return false;
    }

    granny_file_info* animInfo = GrannyGetFileInfo(animFile);
    if (!animInfo || animInfo->AnimationCount <= 0 || !animInfo->Animations[0])
    {
        Log("[POSE] animation file has no usable animation");
        cleanup();
        return false;
    }

    granny_animation* animation = animInfo->Animations[0];
    const std::unordered_set<std::string> modelBoneSet = BuildSkeletonBoneSet(skeleton);
    int trackCount = 0;
    const int boundTrackCount = CountMatchingAnimationTracks(animation, modelBoneSet, &trackCount);
    LogFormat("[POSE] binding source: model='%s' skeleton='%s' bones=%d animation='%s' tracks=%d",
        Safe(model->Name),
        Safe(skeleton->Name),
        skeleton->BoneCount,
        Safe(animation->Name),
        trackCount);

    modelInstance = GrannyInstantiateModel(model);
    if (!modelInstance)
    {
        Log("[POSE] GrannyInstantiateModel failed");
        cleanup();
        return false;
    }

    control = GrannyPlayControlledAnimation(0.0f, animation, modelInstance);
    if (!control)
    {
        Log("[POSE] GrannyPlayControlledAnimation failed");
        cleanup();
        return false;
    }

    GrannySetControlWeight(control, 1.0f);
    GrannySetControlClock(control, timeSeconds);
    GrannySetModelClock(modelInstance, timeSeconds);
    LogFormat("[POSE] bound animation to model: %d bones bound (%d tracks, %d unmatched)",
        boundTrackCount,
        trackCount,
        trackCount - boundTrackCount);

    localPose = GrannyNewLocalPose(skeleton->BoneCount);
    worldPose = GrannyNewWorldPose(skeleton->BoneCount);
    if (!localPose || !worldPose)
    {
        Log("[POSE] failed to allocate local/world pose");
        cleanup();
        return false;
    }

    GrannySampleModelAnimations(modelInstance, 0, skeleton->BoneCount, localPose);
    GrannyBuildWorldPose(skeleton, 0, skeleton->BoneCount, localPose, nullptr, worldPose);
    LogFormat("[POSE] sampled world pose at t=%.6f: %d matrices", timeSeconds, skeleton->BoneCount);

    const int matricesToLog = std::min(skeleton->BoneCount, 6);
    for (int boneIndex = 0; boneIndex < matricesToLog; ++boneIndex)
    {
        const granny_bone& bone = skeleton->Bones[boneIndex];
        LogPoseMatrix("bone", boneIndex, bone.Name, GrannyGetWorldPose4x4(worldPose, boneIndex));
    }

    granny_matrix_4x4* compositeArray = GrannyGetWorldPoseComposite4x4Array(worldPose);
    LogFormat("[POSE] composite matrix array: %s", compositeArray ? "available" : "null");
    for (int boneIndex = 0; boneIndex < matricesToLog; ++boneIndex)
    {
        const granny_bone& bone = skeleton->Bones[boneIndex];
        LogPoseMatrix("composite", boneIndex, bone.Name, GrannyGetWorldPoseComposite4x4(worldPose, boneIndex));
    }

    Log("[POSE] GrannyGetWorldPoseComposite4x4Array is available for the next skinning/deform step");
    cleanup();
    return true;
}

void GrannyModel::Destroy()
{
    if (m_impl)
    {
        if (m_impl->file)
            GrannyFreeFile(m_impl->file);
        delete m_impl;
        m_impl = nullptr;
    }
}
