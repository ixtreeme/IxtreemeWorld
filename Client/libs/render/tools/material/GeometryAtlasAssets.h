#pragma once
#include "GeometryBaker.h"
#include "AssetLibrary.h"

namespace material_tool {
bool CreateGeometryAtlasAsset(AssetLibrary& library,const std::string& sourceGuid,std::uint64_t sourceHash,
    unsigned submesh,const GeometryMesh& atlas,AssetLibrary::Entry& result,std::string& error);
}
