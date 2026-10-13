#pragma once
#include "MapEditorTypes.h"
#include "GeometryBaker.h"
#include <filesystem>

namespace material_tool {
std::uint64_t SmartMaterialModelHash(const std::filesystem::path& path, std::string& error);
GeometryBakeSettings SelectedMeshBakeSettings(const MeshRendererEditorState& mesh, unsigned resolution);
SmartMaterialAssignment CaptureSmartMaterialTarget(const MeshRendererEditorState& mesh);
// Applies only the baked slot. Concurrent model/transform/slot changes reject the result.
bool ApplySmartMaterialAssignment(MeshSceneEntity& mesh, const SmartMaterialAssignment& assignment, std::string& error);
}
