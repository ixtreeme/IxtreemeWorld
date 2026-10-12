#pragma once
#include "tools/material/GeometryBaker.h"
#include <filesystem>
#include <fstream>
namespace geometry_fixtures {
using namespace material_tool;
inline void Quad(GeometryMesh& mesh,std::array<GeometryVec3,4> positions,GeometryVec3 normal,float u,float v,float width,float height) {
    const auto first=static_cast<std::uint32_t>(mesh.vertices.size());
    const std::array<std::array<float,2>,4> uv{{{u,v},{u+width,v},{u+width,v+height},{u,v+height}}};
    for(int k=0;k<4;++k)mesh.vertices.push_back({positions[k],normal,uv[k]});
    for(unsigned k:{0U,1U,2U,0U,2U,3U})mesh.indices.push_back(first+k);
}
inline GeometryMesh Plane(float margin=0) {
    GeometryMesh m;Quad(m,{{{-1,0,-1},{-1,0,1},{1,0,1},{1,0,-1}}},{0,1,0},margin,margin,1-2*margin,1-2*margin);return m;
}
inline GeometryMesh Box(bool inward=false) {
    GeometryMesh mesh;
    const GeometryVec3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(unsigned face=0;face<6;++face) {
        auto n=normals[face];GeometryVec3 u=face<2?GeometryVec3{0,1,0}:GeometryVec3{1,0,0};
        GeometryVec3 v{n[1]*u[2]-n[2]*u[1],n[2]*u[0]-n[0]*u[2],n[0]*u[1]-n[1]*u[0]};
        std::array<GeometryVec3,4> p{};const float signs[][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
        for(int i=0;i<4;++i)for(int k=0;k<3;++k)p[i][k]=.5f*(n[k]+signs[i][0]*u[k]+signs[i][1]*v[k]);
        if(inward)for(float& x:n)x=-x;
        const auto before=mesh.indices.size();Quad(mesh,p,n,.02f+(face%3)/3.0f,.02f+(face/3)*.5f,.29f,.46f);
        if(inward)for(size_t i=before;i<mesh.indices.size();i+=3)std::swap(mesh.indices[i+1],mesh.indices[i+2]);
    }return mesh;
}
inline GeometryMesh Corner() {
    GeometryMesh mesh;
    Quad(mesh,{{{0,0,0},{0,0,1},{1,0,1},{1,0,0}}},{0,1,0},.02f,.02f,.46f,.96f);
    Quad(mesh,{{{0,0,0},{0,1,0},{0,1,1},{0,0,1}}},{1,0,0},.52f,.02f,.46f,.96f);return mesh;
}
inline bool WriteObj(const std::filesystem::path& path,const GeometryMesh& mesh) {
    std::ofstream file(path);file<<"o GeometryFixture\n";
    for(const auto& v:mesh.vertices)file<<"v "<<v.position[0]<<' '<<v.position[1]<<' '<<v.position[2]<<'\n';
    for(const auto& v:mesh.vertices)file<<"vt "<<v.uv[0]<<' '<<v.uv[1]<<'\n';
    for(const auto& v:mesh.vertices)file<<"vn "<<v.normal[0]<<' '<<v.normal[1]<<' '<<v.normal[2]<<'\n';
    for(size_t i=0;i<mesh.indices.size();i+=3){file<<'f';for(unsigned j=0;j<3;++j){const auto index=mesh.indices[i+j]+1;file<<' '<<index<<'/'<<index<<'/'<<index;}file<<'\n';}
    file.close();return !file.fail();
}
}
