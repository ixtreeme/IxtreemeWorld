#include "SmartMaterialApplication.h"
#include "MeshTransform.h"
#include "asset/AssetDatabase.h"
#include "asset/MaterialAssetManager.h"
#include "RendererGeometry.h"
#include <algorithm>

namespace material_tool {
std::uint64_t SmartMaterialModelHash(const std::filesystem::path& path,std::string& error)
{
    const auto imported=ImportRendererGeometry(path);
    if(!imported.success || imported.hasSkeletal || imported.meshes.empty()){error="Static source model is unavailable or changed";return 0;}
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto& source:imported.meshes){GeometryMesh mesh;mesh.indices=source.indices;
        for(const auto& v:source.vertices)mesh.vertices.push_back({{v.position[0],v.position[1],v.position[2]},{v.normal[0],v.normal[1],v.normal[2]},{v.uv[0],v.uv[1]}});
        hash=(hash^GeometryMeshHash(mesh))*1099511628211ULL;hash=(hash^source.materialIndex)*1099511628211ULL;
    }
    error.clear();return hash;
}
GeometryBakeSettings SelectedMeshBakeSettings(const MeshRendererEditorState& mesh, unsigned resolution)
{
    GeometryBakeSettings settings; settings.resolution = resolution;
    const auto model = MeshModelMatrix(mesh.position, mesh.rotation, mesh.scale);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            settings.objectToWorld[row * 4 + col] = model.m[col * 4 + row];
    return settings;
}
SmartMaterialAssignment CaptureSmartMaterialTarget(const MeshRendererEditorState& mesh)
{
    SmartMaterialAssignment result;
    result.entityId = mesh.id; result.slot = mesh.selectedMaterialSlot;
    result.meshAssetId = mesh.meshAssetId; result.meshAssetPath = mesh.meshAssetPath;
    if (result.slot < mesh.materialSlots.size()) result.previousMaterialGuid = mesh.materialSlots[result.slot];
    if (result.slot < mesh.materialOverrides.size()) result.previousOverride = mesh.materialOverrides[result.slot];
    std::copy_n(mesh.position, 3, result.position.begin());
    std::copy_n(mesh.rotation, 3, result.rotation.begin());
    std::copy_n(mesh.scale, 3, result.scale.begin());
    return result;
}
bool ApplySmartMaterialAssignment(MeshSceneEntity& mesh, const SmartMaterialAssignment& assignment, std::string& error)
{
    if (mesh.id != assignment.entityId || mesh.skinned || mesh.meshAssetId != assignment.meshAssetId ||
        mesh.meshAssetPath != assignment.meshAssetPath ||
        !std::equal(assignment.position.begin(), assignment.position.end(), mesh.position) ||
        !std::equal(assignment.rotation.begin(), assignment.rotation.end(), mesh.rotation) ||
        !std::equal(assignment.scale.begin(), assignment.scale.end(), mesh.scale))
    { error = "Model or transform changed during baking; the material remains in Asset Browser. Bake & Apply again."; return false; }
    if(!assignment.sourceModelGuid.empty()) {
        const auto source=Guid::fromString(assignment.sourceModelGuid);
        const auto sourcePath=source?AssetDatabase::Instance().resolveGuid(*source):std::nullopt;
        if(!sourcePath || !assignment.sourceModelHash || SmartMaterialModelHash(*sourcePath,error)!=assignment.sourceModelHash)
        {error="Source model changed during baking; the original model and material were kept.";return false;}
    }
    const auto previous = assignment.slot < mesh.materialSlots.size() ? mesh.materialSlots[assignment.slot] : std::string{};
    const auto previousOverride = assignment.slot < mesh.materialOverrides.size() ? mesh.materialOverrides[assignment.slot] : MeshSceneEntity::MaterialOverride{};
    if (previous != assignment.previousMaterialGuid || previousOverride != assignment.previousOverride || assignment.slot >= 4096)
    { error = "Material slot changed during baking; the baked material remains in Asset Browser."; return false; }
    const auto guid = Guid::fromString(assignment.materialGuid);
    const auto path = guid ? AssetDatabase::Instance().resolveGuid(*guid) : std::nullopt;
    if (!path || detectAssetType(*path) != AssetType::Material || !MaterialAssetManager::Instance().getOrLoad(*guid))
    { error = "Baked material is unavailable; the model material was kept."; return false; }
    if(!assignment.replacementModelGuid.empty()) {
        const auto model=Guid::fromString(assignment.replacementModelGuid);
        const auto modelPath=model?AssetDatabase::Instance().resolveGuid(*model):std::nullopt;
        if(!modelPath || detectAssetType(*modelPath)!=AssetType::Model || assignment.replacementModelPath.empty())
        {error="Automatic UV model is unavailable; the original model was kept.";return false;}
        mesh.meshAssetId=assignment.replacementModelGuid;mesh.meshAssetPath=assignment.replacementModelPath;
    }
    if (mesh.materialSlots.size() <= assignment.slot) mesh.materialSlots.resize(assignment.slot + 1);
    mesh.materialSlots[assignment.slot] = assignment.materialGuid;
    // Slot overrides would otherwise change the appearance of the newly baked material.
    if (assignment.slot < mesh.materialOverrides.size()) mesh.materialOverrides[assignment.slot] = {};
    error.clear(); return true;
}
}
