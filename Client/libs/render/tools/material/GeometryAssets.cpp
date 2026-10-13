#include "GeometryAssets.h"
#include "MaterialGraphIO.h"
#include "MaterialInstance.h"
#include "asset/AssetDatabase.h"
#include "RendererGeometry.h"
#include "SmartMaterialApplication.h"
#include <algorithm>
#include <fstream>

namespace material_tool {
namespace {
GeometryMesh Convert(const AssimpImporter::StaticMeshData& mesh) {
    GeometryMesh result;result.indices=mesh.indices;result.vertices.reserve(mesh.vertices.size());
    for(const auto& v:mesh.vertices)result.vertices.push_back({{v.position[0],v.position[1],v.position[2]},{v.normal[0],v.normal[1],v.normal[2]},{v.uv[0],v.uv[1]}});
    return result;
}
// One submesh, or the submeshes of one material slot joined in model order. A single submesh
// yields the same mesh either way, so its content hash does not depend on how it was selected.
bool Gather(const AssimpImporter::ImportResult& imported,bool bySlot,unsigned key,GeometryMesh& mesh,std::vector<unsigned>& indices,std::string& name,std::string& error) {
    std::vector<unsigned> found;
    if(!bySlot){if(key>=imported.meshes.size()){error="Selected geometry submesh index no longer exists";return false;}found={key};}
    else {
        for(unsigned index=0;index<imported.meshes.size();++index)if(imported.meshes[index].materialIndex==key)found.push_back(index);
        if(found.empty()){error="Material slot "+std::to_string(key)+" has no geometry in this model";return false;}
    }
    GeometryMesh joined;
    for(const auto index:found) {
        const auto part=Convert(imported.meshes[index]);const auto base=static_cast<std::uint32_t>(joined.vertices.size());
        joined.vertices.insert(joined.vertices.end(),part.vertices.begin(),part.vertices.end());
        for(const auto vertex:part.indices)joined.indices.push_back(base+vertex);
    }
    name=imported.meshes[found.front()].name;if(found.size()>1)name+=" (+"+std::to_string(found.size()-1)+" sharing its material)";
    mesh=std::move(joined);indices=std::move(found);return true;
}
bool Import(const std::filesystem::path& path,AssimpImporter::ImportResult& result,std::string& error) {
    if(detectAssetType(path)!=AssetType::Model){error="Geometry source must be a model asset";return false;}
    std::error_code ec;if(!std::filesystem::is_regular_file(path,ec)){error="Geometry source mesh is missing";return false;}
    result=ImportRendererGeometry(path);
    if(!result.success || result.meshes.empty()){error="Cannot import geometry mesh: "+result.errorMessage;return false;}
    if(result.hasSkeletal){error="Geometry baking currently supports static submeshes; bind-pose/skinned baking is not implemented";return false;}
    return true;
}
bool Read(const std::filesystem::path& path,GeometryBake& bake,std::string& sourceGuid,std::string& error) {
    std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
    if(ec || size>28*1024*1024){error="Cannot read geometry cache (maximum 28 MB)";return false;}
    std::ifstream input(path,std::ios::binary);std::string bytes(static_cast<size_t>(size),'\0');
    if(!input || !input.read(bytes.data(),static_cast<std::streamsize>(size)) || input.peek()!=std::char_traits<char>::eof()){error="Geometry cache could not be read completely";return false;}
    if(!DecodeGeometryBake(bytes,bake,error))return false;
    const auto deps=AssetDatabase::Instance().loadDependencies(path);
    if(deps.size()!=1){error="Geometry cache requires exactly one native model GUID dependency";return false;}
    sourceGuid=deps.front().toString();return true;
}
}
GraphGeometryCache::Stamp GraphGeometryCache::Current(const std::string& guid) {
    Stamp stamp;if(const auto parsed=Guid::fromString(guid))if(const auto path=AssetDatabase::Instance().resolveGuid(*parsed)) {
        stamp.path=*path;std::error_code ec;stamp.exists=std::filesystem::is_regular_file(*path,ec);
        if(stamp.exists){stamp.time=std::filesystem::last_write_time(*path,ec);if(ec)stamp.exists=false;stamp.bytes=std::filesystem::file_size(*path,ec);if(ec)stamp.exists=false;}
        auto meta=*path;meta+=L".meta";
        if(std::filesystem::is_regular_file(meta,ec)){stamp.metaTime=std::filesystem::last_write_time(meta,ec);stamp.metaBytes=std::filesystem::file_size(meta,ec);}
    }return stamp;
}
void GraphGeometryCache::Clear() {mesh_={};bake_={};meshGuid_.clear();bakeGuid_.clear();name_.clear();error_.clear();meshStamp_={};bakeStamp_={};meshIndices_.clear();meshIndex_=meshCount_=slot_=0;bySlot_=meshValid_=bakeValid_=false;}
bool GraphGeometryCache::Select(const std::filesystem::path& path,unsigned index,std::string& error) {
    error.clear();AssimpImporter::ImportResult imported;if(!Import(path,imported,error))return false;
    GeometryMesh mesh;std::vector<unsigned> indices;std::string name;if(!Gather(imported,false,index,mesh,indices,name,error))return false;
    const auto guid=AssetDatabase::Instance().getOrCreateGuid(path).toString();if(!IsMaterialAssetGuid(guid)){error="Cannot register geometry model GUID";return false;}
    Clear();mesh_=std::move(mesh);meshGuid_=guid;meshStamp_=Current(guid);meshIndices_=std::move(indices);meshIndex_=index;meshCount_=static_cast<unsigned>(imported.meshes.size());name_=std::move(name);meshValid_=true;return true;
}
bool GraphGeometryCache::SelectMaterialSlot(const std::filesystem::path& path,unsigned slot,std::string& error,std::uint64_t* modelHash) {
    error.clear();AssimpImporter::ImportResult imported;if(!Import(path,imported,error))return false;
    GeometryMesh mesh;std::vector<unsigned> indices;std::string name;if(!Gather(imported,true,slot,mesh,indices,name,error))return false;
    const auto guid=AssetDatabase::Instance().getOrCreateGuid(path).toString();if(!IsMaterialAssetGuid(guid)){error="Cannot register geometry model GUID";return false;}
    Clear();mesh_=std::move(mesh);meshGuid_=guid;meshStamp_=Current(guid);meshIndex_=indices.front();meshIndices_=std::move(indices);slot_=slot;bySlot_=true;
    meshCount_=static_cast<unsigned>(imported.meshes.size());name_=std::move(name);meshValid_=true;
    if(modelHash)*modelHash=SmartMaterialModelHash(imported);
    return true;
}
bool GraphGeometryCache::Bind(const std::string& guid,std::string& error) {
    error.clear();const auto stamp=Current(guid);
    if(!stamp.exists || detectAssetType(stamp.path)!=AssetType::GeometryBake){error="Missing Geometry Bake asset GUID: "+guid;return false;}
    GeometryBake bake;std::string sourceGuid;if(!Read(stamp.path,bake,sourceGuid,error))return false;
    const auto source=Current(sourceGuid);AssimpImporter::ImportResult imported;
    if(!source.exists || !Import(source.path,imported,error)){if(error.empty())error="Geometry cache source model GUID is missing";return false;}
    GeometryMesh mesh;std::vector<unsigned> indices;std::string name,unused;unsigned index=0,slot=0;bool found=false,bySlot=false;
    for(;index<imported.meshes.size();++index)if(Gather(imported,false,index,mesh,indices,name,unused) && GeometryMeshHash(mesh)==bake.meshHash){found=true;break;}
    // A cache baked for a material slot shared by several submeshes covers their joined surface.
    for(;!found && slot<imported.materials.size();++slot)if(Gather(imported,true,slot,mesh,indices,name,unused) && indices.size()>1 && GeometryMeshHash(mesh)==bake.meshHash){found=bySlot=true;index=indices.front();break;}
    if(!found){error="Geometry cache is stale: source mesh content changed; rebake required";return false;}
    if(Current(sourceGuid)!=source || Current(guid)!=stamp){error="Geometry dependencies changed while loading; retry";return false;}
    Clear();mesh_=std::move(mesh);bake_=std::move(bake);meshGuid_=sourceGuid;bakeGuid_=guid;meshStamp_=source;bakeStamp_=stamp;meshIndices_=std::move(indices);meshIndex_=index;slot_=slot;bySlot_=bySlot;meshCount_=static_cast<unsigned>(imported.meshes.size());name_=std::move(name);meshValid_=bakeValid_=true;return true;
}
bool GraphGeometryCache::Refresh(bool force) {
    if(meshGuid_.empty())return false;
    const auto source=Current(meshGuid_),cache=Current(bakeGuid_);
    if(!force && source==meshStamp_ && cache==bakeStamp_)return false;
    meshStamp_=source;bakeStamp_=cache;meshValid_=bakeValid_=false;error_.clear();
    AssimpImporter::ImportResult imported;
    if(!source.exists || !Import(source.path,imported,error_)){if(error_.empty())error_="Geometry source model GUID is missing";return true;}
    if(!Gather(imported,bySlot_,bySlot_?slot_:meshIndex_,mesh_,meshIndices_,name_,error_)){if(!bySlot_)error_="Geometry submesh was removed; select a submesh again";return true;}
    meshIndex_=meshIndices_.front();meshValid_=true;meshCount_=static_cast<unsigned>(imported.meshes.size());
    if(!bakeGuid_.empty()) {
        if(!cache.exists){error_="Geometry Bake asset is missing";return true;}
        GeometryBake updated;std::string guid;
        if(!Read(cache.path,updated,guid,error_))return true;
        if(guid!=meshGuid_){error_="Geometry Bake source GUID changed; bind the asset again";return true;}
        bake_=std::move(updated);
    }
    if(bake_.texels.empty()){error_="Missing Geometry Bake: start a bake for the selected mesh";return true;}
    if(GeometryMeshHash(mesh_)!=bake_.meshHash){error_="Geometry cache is stale: source mesh content changed; rebake required";return true;}
    bakeValid_=true;return true;
}
bool GraphGeometryCache::Accept(GeometryBake bake,std::string& error) {
    Refresh();if(!meshValid_){error=error_.empty()?"Select a geometry mesh first":error_;return false;}
    if(!ValidateGeometryBake(bake,error))return false;
    if(GeometryMeshHash(mesh_)!=bake.meshHash){error="Source mesh changed during geometry baking; rebake required";return false;}
    bake_=std::move(bake);bakeValid_=true;bakeGuid_.clear();bakeStamp_={};error_.clear();return true;
}
bool GraphGeometryCache::Save(const std::filesystem::path& path,std::string& error) {
    Refresh();if(!bakeValid_){error=error_.empty()?"Bake the selected mesh before saving a cache":error_;return false;}
    if(detectAssetType(path)!=AssetType::GeometryBake){error="Use .ixgeometrybake for a geometry cache";return false;}
    std::string bytes;if(!EncodeGeometryBake(bake_,bytes,error) || !SaveMaterialTextAtomic(path,bytes,error))return false;
    auto& database=AssetDatabase::Instance();const auto guid=database.getOrCreateGuid(path);
    const auto source=Guid::fromString(meshGuid_);
    if(!source || !database.writeDependencies(path,{*source})){error="Geometry cache saved, but native dependency metadata failed";return false;}
    bakeGuid_=guid.toString();bakeStamp_=Current(bakeGuid_);return true;
}
}
