#include "tools/material/MaterialGraphAssets.h"
#include "tools/material/MaterialGraphIO.h"
#include "tools/material/GeometryAssets.h"
#include "GeometryFixtures.h"
#include "asset/MaterialAssetManager.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace material_tool;
namespace {
int failures=0;
void Check(bool ok,const char* message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';failures+=!ok;}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("ixtreeme-graph-bake-"+generateGuidV4().toString());
    std::filesystem::create_directories(root);AssetDatabase::Instance().scan(root);AssetLibrary library(root,root/"Assets");Check(library.Initialize(),"isolated test project initializes");
    auto graph=MaterialGraphDocument::Example();graph.FindNode(1)->value={.25f,.5f,.75f,1};graph.FindNode(2)->value[0]=.6f;
    AssetLibrary::Entry material;std::string error;
    const bool baked=PublishBakedMaterial(library,graph,{},16,"Test material",material,error);if(!baked)std::cout<<error<<'\n';
    Check(baked && material.category==AssetLibrary::Category::Material && !material.guid.empty(),"UV graph publishes a real GUID material to the Asset Browser");
    if(baked) {
        const auto guid=Guid::fromString(material.guid);const auto* loaded=guid?MaterialAssetManager::Instance().getOrLoad(*guid):nullptr;
        Check(loaded && loaded->baseColorTexture && loaded->normalTexture && loaded->metallicRoughnessTexture && loaded->heightTexture && loaded->emissiveTexture,"published material uses baked color, normal, ORM, height and emissive maps");
        if(loaded && loaded->metallicRoughnessTexture) {
            const auto orm=AssetDatabase::Instance().resolveGuid(*loaded->metallicRoughnessTexture);int w=0,h=0,c=0;auto* pixels=orm?stbi_load(orm->string().c_str(),&w,&h,&c,4):nullptr;
            Check(pixels && w==16 && pixels[0]==255 && pixels[1]==153 && pixels[2]==0,"runtime ORM uses AO/Roughness/Metallic in RGB order");stbi_image_free(pixels);
        }
        Check(std::filesystem::exists(library.AbsolutePath(material).parent_path()/"source.materialgraph"),"bake retains its editable source graph");
        AssetLibrary reopened(root,root/"Assets");Check(reopened.InitializeReadOnly() && reopened.FindById(material.id).has_value(),"published material survives manifest reload");
    }
    const std::uint8_t image[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};const auto file=root/"sample.png";
    stbi_write_png(file.string().c_str(),2,2,4,image,8);AssetLibrary::Entry texture;Check(library.Import(AssetLibrary::Category::Texture,file,texture,error),"texture asset imports for graph binding");
    GraphTextureCache cache;cache.Configure(&library,root);GraphNode node;node.id=23;node.resource=library.AssetRelativePath(texture);node.assetGuid=texture.guid;node.assetId=texture.id;
    auto red=cache.Sample(node,"",.25f,.25f).rgba,wrap=cache.Sample(node,"",-.75f,1.25f).rgba;Check(red[0]==1&&red[1]==0&&red==wrap,"texture cache bilinear repeat handles negative UVs");
    const auto middle=cache.Sample(node,"",.5f,.5f).rgba;Check(middle[0]==.5f&&middle[1]==.5f&&middle[2]==.5f,"texture cache filters four texels bilinearly");
    GraphNode other;other.id=24;const auto overrideColor=cache.Sample(other,library.AssetRelativePath(texture),.25f,.25f).rgba;Check(overrideColor==red,"project-relative instance texture override resolves correctly");
    AssetLibrary::Entry moved;Check(library.MoveAssetToSubpath(texture.id,"Moved",moved,error),"texture moves within the project");cache.Clear();Check(cache.Sample(node,"",.25f,.25f).rgba==red,"stable asset binding finds moved texture without rewriting the graph");
    GraphNode missing;missing.id=25;missing.resource="missing.png";Check(cache.Sample(missing,"",0,0).rgba[1]==0 && !cache.Error().empty(),"missing texture reports its node and error");
    cache.BeginEvaluation();GraphNode wrongGuid=node;wrongGuid.id=26;wrongGuid.assetGuid="missing-guid";wrongGuid.resource=library.AssetRelativePath(moved);
    Check(cache.Sample(wrongGuid,"",.25f,.25f).rgba!=red && !cache.Error().empty(),"missing GUID cannot fall back to a valid stale texture path");
    cache.BeginEvaluation();Check(cache.Sample(other,"guid:"+moved.guid,.25f,.25f).rgba==red && cache.Error().empty(),"GUID-based instance texture override resolves after a move");
    GraphTextureCache noLibrary;noLibrary.Configure(nullptr,root);
    Check(noLibrary.Sample(node,"",.25f,.25f).rgba==red && noLibrary.Error().empty(),"texture GUID resolves through the native database without an AssetLibrary pointer");
    wrongGuid.resource=library.AbsolutePath(moved).string();
    Check(noLibrary.Sample(wrongGuid,"",.25f,.25f).rgba!=red && !noLibrary.Error().empty(),"missing GUID remains authoritative when no AssetLibrary is configured");
    AssetLibrary::Entry renamed;
    Check(library.RenameAsset(moved.id,"renamed_texture",false,renamed,error),"texture rename succeeds through the native library workflow");
    noLibrary.Clear();
    Check(noLibrary.Sample(node,"",.25f,.25f).rgba==red && noLibrary.Error().empty(),"native GUID lookup follows texture rename immediately without a watcher event");
    const auto count=library.Entries().size();Check(!PublishBakedMaterial(library,MaterialGraphDocument::MossyRock(),{},16,"Invalid UV",material,error)&&library.Entries().size()==count,"world-dependent material is rejected before writing assets");
    auto subgraph=MaterialSubgraph::Example();const auto subgraphFile=root/"scale.ixmaterialsubgraph";
    SaveMaterialTextAtomic(subgraphFile,subgraph.ToJson(),error);const auto subgraphGuid=AssetDatabase::Instance().getOrCreateGuid(subgraphFile);
    auto subgraphMaterial=MaterialGraphDocument::Example();const auto call=subgraphMaterial.AddNode(NodeKind::Subgraph,{});
    BindMaterialSubgraph(subgraphMaterial,call,subgraphGuid.toString(),subgraph,error);subgraphMaterial.Connect(PinId(call,1),PinId(3,1),error);
    ParameterOverrides overrides;overrides.values["Subgraph_"+std::to_string(call)+"/Scale"]={.5f,0,0,0};
    const bool subgraphBaked=PublishBakedMaterial(library,subgraphMaterial,overrides,16,"Subgraph material",material,error);
    Check(subgraphBaked,"GUID-backed subgraph with scoped instance parameters publishes a real native material");
    if(subgraphBaked) {
        const auto folder=library.AbsolutePath(material).parent_path();std::string text;MaterialGraphDocument effective,authored;
        const bool sources=LoadMaterialText(folder/"source.materialgraph",text,error)&&effective.FromJson(text,error)&&LoadMaterialText(folder/"definition.materialgraph",text,error)&&authored.FromJson(text,error);
        Check(sources && authored==subgraphMaterial && std::none_of(effective.nodes.begin(),effective.nodes.end(),[](const auto& n){return n.kind==NodeKind::Subgraph;}),"bake retains authored definition and a self-contained effective source snapshot");
        MaterialGraphProgram source;std::vector<std::array<GraphValue,14>> work;
        Check(source.Compile(effective,error)&&std::abs(source.Evaluate({}, {}, {},work).roughness-.25f)<.0001f,"effective source retains scoped override values without resolving external subgraphs again");
        const auto dependencies=AssetDatabase::Instance().loadDependencies(folder/"definition.materialgraph");
        Check(std::find(dependencies.begin(),dependencies.end(),subgraphGuid)!=dependencies.end(),"baked authored definition tracks native subgraph dependencies");
    }
    const auto modelFile=root/"bake-box.obj",geometryFile=root/"box.ixgeometrybake";
    Check(geometry_fixtures::WriteObj(modelFile,geometry_fixtures::Box()),"real OBJ fixture writes with UV seams");
    GraphGeometryCache geometry;GeometryBake bake;GeometryBakeSettings settings;settings.resolution=32;settings.rayDistance=3;
    const bool selected=geometry.Select(modelFile,0,error);
    Check(selected && geometry.Mesh() && geometry.MeshCount()==1,"geometry bridge imports a native model submesh through Assimp");
    if(selected) {
        const bool geometryBaked=BakeGeometry(*geometry.Mesh(),settings,bake,error) && geometry.Accept(bake,error) && geometry.Save(geometryFile,error);
        if(!geometryBaked)std::cout<<error<<'\n';
        Check(geometryBaked && detectAssetType(geometryFile)==AssetType::GeometryBake,"real mesh cache bakes, validates and saves with native asset type");
        if(geometryBaked) {
            const auto geometryGuid=geometry.BakeGuid(),modelGuid=geometry.MeshGuid();GraphGeometryCache bound;
            Check(bound.Bind(geometryGuid,error) && bound.Bake() && bound.Bake()->meshHash==bake.meshHash,"cache reload verifies actual source content against its GUID dependency");
            Check(AssetDatabase::Instance().loadDependencies(geometryFile)==std::vector<Guid>{*Guid::fromString(modelGuid)},"geometry cache records native source mesh dependency");
            auto stone=MaterialGraphDocument::WeatheredStone();
            const bool stoneBaked=PublishBakedMaterial(library,stone,{},32,"Mesh Weathered Stone",material,error,{},geometry.Bake(),geometryGuid);
            if(!stoneBaked)std::cout<<error<<'\n';Check(stoneBaked,"Weathered Stone publishes real mesh-dependent color/normal/ORM/height material");
            if(stoneBaked) {
                const auto folder=library.AbsolutePath(material).parent_path();GraphGeometryCache snapshot;
                const auto snapshotGuid=AssetDatabase::Instance().resolvePath(folder/"geometry.ixgeometrybake");
                Check(snapshotGuid && snapshot.Bind(snapshotGuid->toString(),error) && snapshot.Bake()->meshHash==bake.meshHash,"published material retains exact geometry snapshot with native dependency");
                const auto sourceDependencies=AssetDatabase::Instance().loadDependencies(folder/"source.materialgraph");
                Check(snapshotGuid && std::find(sourceDependencies.begin(),sourceDependencies.end(),*snapshotGuid)!=sourceDependencies.end(),"effective source tracks its geometry snapshot GUID");
                const auto* native=MaterialAssetManager::Instance().getOrLoad(*Guid::fromString(material.guid));
                Check(native && native->baseColorTexture && native->normalTexture && native->metallicRoughnessTexture,"mesh bake uses the existing native material renderer path");
            }
            SmartGenerator generator;const auto generatorFile=root/"wear.ixsmartgenerator";
            const bool generatorCreated=SmartGenerator::Create(SmartGeneratorKind::EdgeWear,generator,error) && SaveMaterialTextAtomic(generatorFile,generator.ToJson(),error);
            Check(generatorCreated,"real generator preset writes a validated versioned asset");
            if(generatorCreated){
                const auto generatorGuid=AssetDatabase::Instance().getOrCreateGuid(generatorFile);GraphSubgraphCache generators;MaterialSubgraph body;
                Check(detectAssetType(generatorFile)==AssetType::SmartGenerator && generators.Resolve(generatorGuid.toString(),body,error) && body==generator.definition,"native generator GUID resolves into the shared subgraph cache");
                auto generated=MaterialGraphDocument::WeatheredStone();const auto generatorCall=AddSmartGenerator(generated,generatorGuid.toString(),generator,generated.layers.back().id,error);
                ParameterOverrides values;values.values["Subgraph_"+std::to_string(generatorCall)+"/WearAmount"]={.4f,0,0,0};
                const bool generatedBake=generatorCall && PublishBakedMaterial(library,generated,values,32,"Generated stone",material,error,{},geometry.Bake(),geometryGuid);
                if(!generatedBake)std::cout<<error<<'\n';Check(generatedBake,"generator layer mask with scoped override publishes real mesh material");
                if(generatedBake){const auto folder=library.AbsolutePath(material).parent_path();const auto dependencies=AssetDatabase::Instance().loadDependencies(folder/"definition.materialgraph");Check(std::find(dependencies.begin(),dependencies.end(),generatorGuid)!=dependencies.end(),"baked definition tracks its native generator preset dependency");std::string text;MaterialGraphDocument effective;MaterialGraphProgram program;
                    Check(LoadMaterialText(folder/"source.materialgraph",text,error) && effective.FromJson(text,error) && program.Compile(effective,error),"published generator source is self-contained ordinary IR input");
                    bool retained=false;for(const auto& n:effective.nodes)if(n.parameter=="Subgraph_"+std::to_string(generatorCall)+"/WearAmount")retained=n.value[0]==.4f;Check(retained,"generator effective snapshot retains scoped instance value");}
            }
            const auto movedModel=root/"moved-box.obj";std::filesystem::rename(modelFile,movedModel);AssetDatabase::Instance().runtimeMove(modelFile,movedModel);
            Check(geometry.Refresh() && geometry.Bake() && geometry.MeshPath()==movedModel,"mesh GUID move refreshes without invalidating unchanged geometric content");
            const auto movedBake=root/"moved-box.ixgeometrybake";std::filesystem::rename(geometryFile,movedBake);AssetDatabase::Instance().runtimeMove(geometryFile,movedBake);
            Check(geometry.Refresh() && geometry.Bake() && bound.Bind(geometryGuid,error),"cache GUID move retains model binding");
            std::string intact;{std::ifstream input(movedBake,std::ios::binary);intact.assign(std::istreambuf_iterator<char>(input),{});}
            SaveMaterialTextAtomic(movedBake,"corrupt",error);Check(geometry.Refresh() && !geometry.Bake() && !geometry.Error().empty(),"corrupt cache invalidates sampling with diagnostic");
            SaveMaterialTextAtomic(movedBake,intact,error);Check(geometry.Refresh() && geometry.Bake(),"repaired cache recovers without changing material definition");
            auto changed=geometry_fixtures::Box();changed.vertices[0].position[0]+=.1f;geometry_fixtures::WriteObj(movedModel,changed);
            Check(geometry.Refresh() && !geometry.Bake() && geometry.Mesh() && geometry.Error().find("stale")!=std::string::npos,"real source edit invalidates cache but retains mesh for rebaking");
            Check(!bound.Bind(geometryGuid,error) && error.find("stale")!=std::string::npos,"stale saved geometry cannot bind to an instance");
            const auto assetCount=library.Entries().size();
            Check(!PublishBakedMaterial(library,stone,{},32,"Stale stone",material,error,{},&bake,geometryGuid) && library.Entries().size()==assetCount,"stale geometry publishing fails before any assets are written");
            Check(!geometry.Accept(bake,error),"completed bake for an old mesh cannot replace the current mesh cache");
        }
    }
    std::filesystem::remove_all(root);std::cout<<"Material graph baking failures: "<<failures<<'\n';return failures?1:0;
}
