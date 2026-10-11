#include "tools/material/MaterialGraphAssets.h"
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
    const auto count=library.Entries().size();Check(!PublishBakedMaterial(library,MaterialGraphDocument::MossyRock(),{},16,"Invalid UV",material,error)&&library.Entries().size()==count,"world-dependent material is rejected before writing assets");
    std::filesystem::remove_all(root);std::cout<<"Material graph baking failures: "<<failures<<'\n';return failures?1:0;
}
