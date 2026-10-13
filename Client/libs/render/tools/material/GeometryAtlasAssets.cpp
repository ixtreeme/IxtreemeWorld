#include "GeometryAtlasAssets.h"
#include "SmartMaterialApplication.h"
#include "asset/AssetDatabase.h"
#include "RendererGeometry.h"
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <algorithm>
#include <memory>

namespace material_tool {
bool CreateGeometryAtlasAsset(AssetLibrary& library,const std::string& sourceGuid,std::uint64_t sourceHash,
    const std::vector<unsigned>& submeshes,const GeometryMesh& atlas,AssetLibrary::Entry& result,std::string& error)
{
    auto& database=AssetDatabase::Instance();const auto guid=Guid::fromString(sourceGuid);
    const auto path=guid?database.resolveGuid(*guid):std::nullopt;
    if(!path || !sourceHash || SmartMaterialModelHash(*path,error)!=sourceHash) {error="Source model changed before UV atlas export";return false;}
    auto imported=ImportRendererGeometry(*path);
    bool valid=imported.success && !imported.hasSkeletal && !submeshes.empty();std::size_t indexCount=0;
    for(std::size_t k=0;valid && k<submeshes.size();++k) {
        valid=submeshes[k]<imported.meshes.size() && (!k || submeshes[k]>submeshes[k-1]) &&
            imported.meshes[submeshes[k]].materialIndex==imported.meshes[submeshes.front()].materialIndex;
        if(valid)indexCount+=imported.meshes[submeshes[k]].indices.size();
    }
    if(!valid || atlas.indices.size()!=indexCount){error="Invalid source for automatic UV model";return false;}
    // The joined surface takes the place of the slot's first submesh; the rest of the slot is now part of it.
    for(std::size_t k=submeshes.size();k-->1;)imported.meshes.erase(imported.meshes.begin()+submeshes[k]);
    auto& replaced=imported.meshes[submeshes.front()];replaced.vertices.clear();replaced.indices=atlas.indices;
    for(const auto& v:atlas.vertices) {AssimpImporter::Vertex vertex;
        std::copy(v.position.begin(),v.position.end(),vertex.position);std::copy(v.normal.begin(),v.normal.end(),vertex.normal);std::copy(v.uv.begin(),v.uv.end(),vertex.uv);
        replaced.vertices.push_back(vertex);
    }
    aiScene scene;scene.mRootNode=new aiNode("SmartMaterialAtlas");
    scene.mNumMeshes=static_cast<unsigned>(imported.meshes.size());scene.mMeshes=new aiMesh*[scene.mNumMeshes]{};
    scene.mRootNode->mNumChildren=scene.mNumMeshes;scene.mRootNode->mChildren=new aiNode*[scene.mNumMeshes]{};
    scene.mNumMaterials=std::max(1U,static_cast<unsigned>(imported.materials.size()));scene.mMaterials=new aiMaterial*[scene.mNumMaterials]{};
    for(unsigned k=0;k<scene.mNumMaterials;++k){scene.mMaterials[k]=new aiMaterial;const aiString name(k<imported.materials.size()?imported.materials[k].name:"Material");scene.mMaterials[k]->AddProperty(&name,AI_MATKEY_NAME);}
    for(unsigned k=0;k<scene.mNumMeshes;++k){const auto& source=imported.meshes[k];auto* mesh=new aiMesh;scene.mMeshes[k]=mesh;
        mesh->mName=aiString(source.name);mesh->mPrimitiveTypes=aiPrimitiveType_TRIANGLE;mesh->mMaterialIndex=std::min(source.materialIndex,scene.mNumMaterials-1);
        mesh->mNumVertices=static_cast<unsigned>(source.vertices.size());mesh->mVertices=new aiVector3D[mesh->mNumVertices];mesh->mNormals=new aiVector3D[mesh->mNumVertices];mesh->mTextureCoords[0]=new aiVector3D[mesh->mNumVertices];mesh->mNumUVComponents[0]=2;
        // Imported geometry is already what the renderer draws, and the native glTF loader reads the
        // file as written. Assimp's glTF exporter stores 1 - V, so V is flipped here to survive it.
        for(unsigned v=0;v<mesh->mNumVertices;++v){const auto& vertex=source.vertices[v];mesh->mVertices[v]={vertex.position[0],vertex.position[1],vertex.position[2]};mesh->mNormals[v]={vertex.normal[0],vertex.normal[1],vertex.normal[2]};mesh->mTextureCoords[0][v]={vertex.uv[0],1-vertex.uv[1],0};}
        mesh->mNumFaces=static_cast<unsigned>(source.indices.size()/3);mesh->mFaces=new aiFace[mesh->mNumFaces];
        for(unsigned f=0;f<mesh->mNumFaces;++f){mesh->mFaces[f].mNumIndices=3;mesh->mFaces[f].mIndices=new unsigned[3]{source.indices[f*3],source.indices[f*3+1],source.indices[f*3+2]};}
        auto* node=new aiNode(source.name);node->mParent=scene.mRootNode;node->mNumMeshes=1;node->mMeshes=new unsigned[1]{k};scene.mRootNode->mChildren[k]=node;
    }
    const auto folder=library.LibraryRoot()/"Models"/"SmartMaterialBakes";std::error_code ec;std::filesystem::create_directories(folder,ec);
    if(ec){error="Cannot create automatic UV model folder";return false;}
    const auto output=folder/(path->stem().string()+"_SmartUV_"+generateGuidV4().toString()+".glb");
    auto temporary=output;temporary+=L".tmp";Assimp::Exporter exporter;
    if(exporter.Export(&scene,"glb2",temporary.string(),aiProcess_CalcTangentSpace)!=aiReturn_SUCCESS){error=std::string("Cannot export automatic UV model: ")+exporter.GetErrorString();std::filesystem::remove(temporary,ec);return false;}
    std::filesystem::rename(temporary,output,ec);if(ec){error="Cannot publish automatic UV model: "+ec.message();std::filesystem::remove(temporary,ec);return false;}
    const auto modelGuid=database.getOrCreateGuid(output);
    if(!database.writeDependencies(output,{*guid}) || !database.writeDefaultMaterials(output,database.loadDefaultMaterials(*path)) || !library.Refresh(error))return false;
    for(const auto& entry:library.Entries())if(entry.guid==modelGuid.toString()){result=entry;return true;}
    error="Automatic UV model is missing from Asset Browser";return false;
}
}
