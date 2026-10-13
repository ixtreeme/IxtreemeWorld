#pragma once
#include "tools/material/GeometryBaker.h"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
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
// One glTF mesh with a primitive and a material per entry. The file stores exactly the given
// positions, normals, UVs and winding, which is what the native glTF renderer reads.
inline bool WriteGltf(const std::filesystem::path& path,const std::vector<GeometryMesh>& primitives) {
    std::string bytes;std::ostringstream views,accessors,parts,materials;
    views<<std::setprecision(std::numeric_limits<float>::max_digits10);accessors<<std::setprecision(std::numeric_limits<float>::max_digits10);
    unsigned view=0;
    const auto append=[&](const void* data,std::size_t size,int target){
        views<<(view?",":"")<<"{\"buffer\":0,\"byteOffset\":"<<bytes.size()<<",\"byteLength\":"<<size<<",\"target\":"<<target<<'}';
        bytes.append(static_cast<const char*>(data),size);return view++;
    };
    for(std::size_t index=0;index<primitives.size();++index) {
        const auto& mesh=primitives[index];std::vector<float> positions,normals,uvs;
        GeometryVec3 low{mesh.vertices[0].position},high=low;
        for(const auto& v:mesh.vertices){
            positions.insert(positions.end(),v.position.begin(),v.position.end());normals.insert(normals.end(),v.normal.begin(),v.normal.end());uvs.insert(uvs.end(),v.uv.begin(),v.uv.end());
            for(int k=0;k<3;++k){low[k]=std::min(low[k],v.position[k]);high[k]=std::max(high[k],v.position[k]);}
        }
        const auto first=append(positions.data(),positions.size()*4,34962);append(normals.data(),normals.size()*4,34962);
        append(uvs.data(),uvs.size()*4,34962);append(mesh.indices.data(),mesh.indices.size()*4,34963);
        accessors<<(index?",":"")<<"{\"bufferView\":"<<first<<",\"componentType\":5126,\"count\":"<<mesh.vertices.size()<<",\"type\":\"VEC3\",\"min\":["<<low[0]<<','<<low[1]<<','<<low[2]<<"],\"max\":["<<high[0]<<','<<high[1]<<','<<high[2]<<"]},"
            <<"{\"bufferView\":"<<first+1<<",\"componentType\":5126,\"count\":"<<mesh.vertices.size()<<",\"type\":\"VEC3\"},"
            <<"{\"bufferView\":"<<first+2<<",\"componentType\":5126,\"count\":"<<mesh.vertices.size()<<",\"type\":\"VEC2\"},"
            <<"{\"bufferView\":"<<first+3<<",\"componentType\":5125,\"count\":"<<mesh.indices.size()<<",\"type\":\"SCALAR\"}";
        parts<<(index?",":"")<<"{\"attributes\":{\"POSITION\":"<<first<<",\"NORMAL\":"<<first+1<<",\"TEXCOORD_0\":"<<first+2<<"},\"indices\":"<<first+3<<",\"material\":"<<index<<'}';
        materials<<(index?",":"")<<"{\"name\":\"Material"<<index<<"\"}";
    }
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for(std::size_t i=0;i<bytes.size();i+=3) {
        const unsigned b0=static_cast<unsigned char>(bytes[i]),b1=i+1<bytes.size()?static_cast<unsigned char>(bytes[i+1]):0,b2=i+2<bytes.size()?static_cast<unsigned char>(bytes[i+2]):0;
        encoded+=alphabet[b0>>2];encoded+=alphabet[((b0&3)<<4)|(b1>>4)];
        encoded+=i+1<bytes.size()?alphabet[((b1&15)<<2)|(b2>>6)]:'=';encoded+=i+2<bytes.size()?alphabet[b2&63]:'=';
    }
    std::ofstream file(path,std::ios::binary);
    file<<"{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],\"materials\":["<<materials.str()
        <<"],\"meshes\":[{\"name\":\"GeometryFixture\",\"primitives\":["<<parts.str()<<"]}],\"buffers\":[{\"byteLength\":"<<bytes.size()
        <<",\"uri\":\"data:application/octet-stream;base64,"<<encoded<<"\"}],\"bufferViews\":["<<views.str()<<"],\"accessors\":["<<accessors.str()<<"]}";
    file.close();return !file.fail();
}
// Triangles with their corner data and winding, independent of vertex sharing and face order.
inline std::vector<std::array<float,24>> SurfaceTriangles(const GeometryMesh& mesh,bool withUv) {
    std::vector<std::array<float,24>> result;
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3) {
        std::array<std::array<float,8>,3> corners{};
        for(unsigned k=0;k<3;++k){const auto& v=mesh.vertices[mesh.indices[i+k]];
            corners[k]={v.position[0],v.position[1],v.position[2],v.normal[0],v.normal[1],v.normal[2],withUv?v.uv[0]:0,withUv?v.uv[1]:0};}
        // Start at the smallest corner without changing the cyclic order.
        std::rotate(corners.begin(),std::min_element(corners.begin(),corners.end()),corners.end());
        std::array<float,24> triangle{};for(unsigned k=0;k<3;++k)std::copy(corners[k].begin(),corners[k].end(),triangle.begin()+k*8);
        result.push_back(triangle);
    }
    std::sort(result.begin(),result.end());return result;
}
inline bool SameSurface(const GeometryMesh& a,const GeometryMesh& b,bool withUv=true,float tolerance=1e-5f) {
    const auto first=SurfaceTriangles(a,withUv),second=SurfaceTriangles(b,withUv);
    if(first.size()!=second.size())return false;
    for(std::size_t i=0;i<first.size();++i)for(unsigned k=0;k<24;++k)if(std::abs(first[i][k]-second[i][k])>tolerance)return false;
    return true;
}
}
