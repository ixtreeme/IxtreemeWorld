#pragma once
#include "MapEditorTypes.h"
#include "GeometryBaker.h"

namespace material_tool {
GeometryBakeSettings SelectedMeshBakeSettings(const MeshRendererEditorState& mesh, unsigned resolution);
SmartMaterialAssignment CaptureSmartMaterialTarget(const MeshRendererEditorState& mesh);
// Applies only the baked slot. Concurrent model/transform/slot changes reject the result.
bool ApplySmartMaterialAssignment(MeshSceneEntity& mesh, const SmartMaterialAssignment& assignment, std::string& error);
}
