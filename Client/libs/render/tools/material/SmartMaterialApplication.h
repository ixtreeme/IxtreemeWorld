#pragma once
#include "MapEditorTypes.h"
#include "GeometryBaker.h"
#include "import_export/AssimpImporter.h"
#include <filesystem>

namespace material_tool {
// Content identity of a whole static model as drawn; 0 when it is not a usable static model.
std::uint64_t SmartMaterialModelHash(const AssimpImporter::ImportResult& imported);
std::uint64_t SmartMaterialModelHash(const std::filesystem::path& path, std::string& error);
GeometryBakeSettings SelectedMeshBakeSettings(const MeshRendererEditorState& mesh, unsigned resolution);
SmartMaterialAssignment CaptureSmartMaterialTarget(const MeshRendererEditorState& mesh);
// Applies only the baked slot. Concurrent model/transform/slot changes reject the result.
bool ApplySmartMaterialAssignment(MeshSceneEntity& mesh, const SmartMaterialAssignment& assignment, std::string& error);
}
