#include "GeometryUvAtlas.h"
#include <xatlas.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace material_tool {
bool GenerateGeometryUvAtlas(const GeometryMesh& mesh,unsigned resolution,GeometryMesh& result,
    std::string& error,const GeometryBakeControl& control)
{
    error.clear();
    if(mesh.vertices.empty() || mesh.vertices.size()>1000000 || mesh.indices.empty() || mesh.indices.size()>3000000 ||
       mesh.indices.size()%3 || resolution<16 || resolution>512) {error="Invalid mesh or resolution for automatic UV atlas";return false;}
    for(const auto& v:mesh.vertices) for(const auto* values:{&v.position,&v.normal})
        if(!std::all_of(values->begin(),values->end(),[](float x){return std::isfinite(x);})) {error="Invalid geometry for automatic UV atlas";return false;}
    if(control.cancelled && control.cancelled()){error="UV atlas cancelled";return false;}
    std::unique_ptr<xatlas::Atlas,decltype(&xatlas::Destroy)> atlas(xatlas::Create(),xatlas::Destroy);
    if(!atlas){error="Cannot allocate automatic UV atlas";return false;}
    xatlas::SetProgressCallback(atlas.get(),[](xatlas::ProgressCategory category,int progress,void* pointer){
        const auto& callbacks=*static_cast<const GeometryBakeControl*>(pointer);
        if(callbacks.progress)callbacks.progress((static_cast<int>(category)+progress/100.0f)/4.0f);
        return !callbacks.cancelled || !callbacks.cancelled();
    },const_cast<GeometryBakeControl*>(&control));
    xatlas::MeshDecl declaration;
    declaration.vertexPositionData=mesh.vertices.data()->position.data();declaration.vertexPositionStride=sizeof(GeometryVertex);
    declaration.vertexNormalData=mesh.vertices.data()->normal.data();declaration.vertexNormalStride=sizeof(GeometryVertex);
    declaration.vertexCount=static_cast<std::uint32_t>(mesh.vertices.size());
    declaration.indexData=mesh.indices.data();declaration.indexCount=static_cast<std::uint32_t>(mesh.indices.size());declaration.indexFormat=xatlas::IndexFormat::UInt32;
    const auto added=xatlas::AddMesh(atlas.get(),declaration,1);
    if(added!=xatlas::AddMeshError::Success){error=std::string("UV atlas input: ")+xatlas::StringForEnum(added);return false;}
    xatlas::PackOptions packing;packing.resolution=resolution;packing.padding=5;
    xatlas::Generate(atlas.get(),{},packing);
    if(control.cancelled && control.cancelled()){error="UV atlas cancelled";return false;}
    if(atlas->meshCount!=1 || atlas->atlasCount!=1 || !atlas->width || !atlas->height) {error="Automatic UV atlas requires a single valid texture page";return false;}
    const auto& output=atlas->meshes[0];
    if(output.vertexCount>1000000 || output.indexCount!=mesh.indices.size()){error="Automatic UV atlas exceeded mesh bounds";return false;}
    GeometryMesh draft;draft.vertices.reserve(output.vertexCount);draft.indices.assign(output.indexArray,output.indexArray+output.indexCount);
    for(std::uint32_t index=0;index<output.vertexCount;++index){const auto& v=output.vertexArray[index];
        if(v.atlasIndex>0 || v.xref>=mesh.vertices.size()){error="Automatic UV atlas requires a single valid texture page";return false;}
        auto vertex=mesh.vertices[v.xref];vertex.uv={v.uv[0]/atlas->width,v.uv[1]/atlas->height};draft.vertices.push_back(vertex);
    }
    // xatlas leaves sliver triangles too small to unwrap without a chart. They keep their place in
    // the surface and get a UV triangle of their own that lies between texel centres at every bake
    // resolution, so it is valid, never sampled and cannot overlap a chart.
    constexpr float Sliver=.4f/512;
    for(std::size_t face=0;face+2<draft.indices.size();face+=3) {
        bool charted=true;for(unsigned corner=0;corner<3;++corner)charted=charted && draft.indices[face+corner]<output.vertexCount && output.vertexArray[draft.indices[face+corner]].atlasIndex==0;
        if(charted)continue;
        const std::array<std::array<float,2>,3> corners{{{0,0},{Sliver,0},{0,Sliver}}};
        for(unsigned corner=0;corner<3;++corner) {
            if(draft.indices[face+corner]>=output.vertexCount){error="Automatic UV atlas returned an invalid face";return false;}
            auto vertex=draft.vertices[draft.indices[face+corner]];vertex.uv=corners[corner];
            draft.indices[face+corner]=static_cast<std::uint32_t>(draft.vertices.size());draft.vertices.push_back(vertex);
        }
    }
    if(draft.vertices.size()>1000000){error="Automatic UV atlas exceeded mesh bounds";return false;}
    result=std::move(draft);return true;
}
GeometryUvAtlasJob::~GeometryUvAtlasJob(){Cancel();if(worker_.joinable())worker_.join();}
bool GeometryUvAtlasJob::Start(GeometryMesh mesh,unsigned resolution){
    if(running_)return false;if(worker_.joinable())worker_.join();ready_=false;progress_=0;result_={};error_.clear();running_=true;
    worker_=std::jthread([this,mesh=std::move(mesh),resolution](std::stop_token token){
        try{success_=GenerateGeometryUvAtlas(mesh,resolution,result_,error_,{[&]{return token.stop_requested();},[&](float v){progress_=v;}});}
        catch(const std::exception& e){success_=false;error_=std::string("UV atlas failed: ")+e.what();}
        ready_=true;running_=false;
    });return true;
}
void GeometryUvAtlasJob::Cancel(){if(worker_.joinable())worker_.request_stop();}
bool GeometryUvAtlasJob::TakeResult(GeometryMesh& result,std::string& error){
    if(!ready_ || running_)return false;if(worker_.joinable())worker_.join();ready_=false;error=error_;if(success_)result=std::move(result_);return true;
}
}
