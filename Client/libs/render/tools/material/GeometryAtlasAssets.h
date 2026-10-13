#pragma once
#include "GeometryBaker.h"
#include "AssetLibrary.h"
#include <vector>

namespace material_tool {
// Writes a copy of the source model whose listed submeshes, which share one material slot, are
// replaced by a single submesh holding their joined surface with the given unique UVs.
bool CreateGeometryAtlasAsset(AssetLibrary& library,const std::string& sourceGuid,std::uint64_t sourceHash,
    const std::vector<unsigned>& submeshes,const GeometryMesh& atlas,AssetLibrary::Entry& result,std::string& error);
}
