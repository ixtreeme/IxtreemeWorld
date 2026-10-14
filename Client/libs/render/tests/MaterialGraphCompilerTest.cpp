#include "tools/material/MaterialGraphImages.h"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#if defined(_WIN32)
#include <process.h>
#endif
using namespace material_tool;
namespace {
int failures=0;
void Check(bool ok,const char* message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';failures+=!ok;}
bool Close(float a,float b){return std::abs(a-b)<1e-4f;}
struct Fixture {
    MaterialGraphDocument graph;
    MaterialGraphProgram program;
    EvaluationContext context;
    ParameterOverrides parameters;
    TextureSampler sample;
    std::vector<std::array<GraphValue,14>> work;
    std::string error;
    std::uint32_t node=0;
    Fixture(NodeKind kind){node=graph.AddNode(kind,{});}
    GraphValue Value(int slot){Check(program.Compile(graph,error,PinId(node,slot)),"numeric test graph compiles");program.Evaluate(context,parameters,sample,work);return program.PreviewValue(work);}
};
void Numbers() {
    for(const auto [kind,expected]:{std::pair{NodeKind::Add,5.0f},std::pair{NodeKind::Subtract,-1.0f},std::pair{NodeKind::Multiply,6.0f},std::pair{NodeKind::Divide,2.0f/3},std::pair{NodeKind::Minimum,2.0f},std::pair{NodeKind::Maximum,3.0f},std::pair{NodeKind::Power,8.0f}}) {
        Fixture f(kind);f.graph.FindNode(f.node)->value={2,3,0,0};Check(Close(f.Value(2).number[0],expected),NodeKindName(kind));
    }
    Fixture divide(NodeKind::Divide);divide.graph.FindNode(divide.node)->value={12,0,0,0};Check(divide.Value(2).number[0]==0,"divide by zero remains finite");
    Fixture clamp(NodeKind::Clamp);clamp.graph.FindNode(clamp.node)->value={12,10,2,0};Check(clamp.Value(3).number[0]==10,"reversed clamp limits are safe");
    Fixture remap(NodeKind::Remap);remap.graph.FindNode(remap.node)->value={0,10,2,4};remap.graph.FindNode(remap.node)->settings[0]=5;Check(Close(remap.Value(5).number[0],3),"remap uses all five inputs");
    Fixture smooth(NodeKind::Smoothstep);smooth.graph.FindNode(smooth.node)->value={.5f,0,1,0};Check(Close(smooth.Value(3).number[0],.5f),"smoothstep midpoint");
    Fixture normal(NodeKind::NormalBlend);const auto n=normal.Value(3).number;Check(Close(n[2],1)&&Close(n[0],0),"flat detail preserves tangent normal");
    Fixture color(NodeKind::Blend);color.graph.FindNode(color.node)->value={.2f,.4f,.6f,1};color.graph.FindNode(color.node)->settings[0]=0;Check(Close(color.Value(3).number[0],.2f),"color blend zero factor preserves base");
    Fixture p(NodeKind::ScalarParameter);p.parameters.values["Parameter_1"]={.86f,0,0,0};Check(Close(p.Value(0).number[0],.86f)&&Close(p.graph.FindNode(p.node)->value[0],.5f),"instance scalar override preserves graph default");
    auto duplicate=p.graph.AddNode(NodeKind::ColorParameter,{});p.graph.FindNode(duplicate)->parameter="Parameter_1";Check(!p.program.Compile(p.graph,p.error),"duplicate exposed parameter names fail compilation");
}
void SpatialAndTextures() {
    Fixture height(NodeKind::HeightMask);height.context.worldPosition={0,5,0,0};Check(Close(height.Value(3).number[0],.5f)&&!height.program.CanBakeUV(),"world height mask is smooth and cannot be flattened into UV");
    Fixture slope(NodeKind::SlopeMask);slope.context.worldNormal={0,1,0,0};Check(slope.Value(3).number[0]==0,"upward normal has no steep-slope mask");slope.context.worldNormal={1,0,0,0};Check(slope.Value(3).number[0]==1,"vertical normal has full steep-slope mask");
    Fixture noise(NodeKind::Noise);noise.context.uv={.32f,.17f,0,0};const float first=noise.Value(2).number[0];Check(first>=0&&first<=1&&noise.program.CanBakeUV(),"UV noise is normalized and bakeable");noise.context.uv[0]+=.0001f;Check(std::abs(first-noise.Value(2).number[0])<.002f,"seeded noise changes continuously");noise.graph.FindNode(noise.node)->value[3]=123;Check(std::abs(first-noise.Value(2).number[0])>.001f,"different noise seed changes mask");
    Fixture uv(NodeKind::TilingOffset);uv.context.uv={.2f,.3f,0,0};uv.graph.FindNode(uv.node)->value={2,3,.1f,.2f};auto mapped=uv.Value(3).number;Check(Close(mapped[0],.5f)&&Close(mapped[1],1.1f),"UV tiling and offset transform both axes");
    Fixture texture(NodeKind::TextureSample);texture.context.uv={.2f,.8f,0,0};texture.sample=[](const auto&,const auto&,float u,float v){return Float4{u,v,.5f,.6f};};auto rgb=texture.Value(2).number;Check(Close(rgb[0],SrgbToLinear(.2f)),"RGB samples decode sRGB");Check(Close(texture.Value(3).number[0],.2f)&&Close(texture.Value(6).number[0],.6f),"individual texture channels preserve raw linear data");
    texture.sample=[](const auto&,const auto&,float,float){return SampledTexture{{.2f,.3f,.4f,.6f},true};};
    Check(Close(texture.Value(2).number[0],.2f)&&Close(texture.Value(3).number[0],.2f),"scene-linear HDR keeps RGB and raw channels unchanged");
    Fixture tri(NodeKind::Triplanar);tri.context.worldPosition={.2f,.3f,.4f,0};tri.context.worldNormal={0,1,0,0};tri.graph.FindNode(tri.node)->srgb=false;tri.sample=[](const auto&,const auto&,float u,float v){return Float4{u,v,.5f,.6f};};auto t=tri.Value(4).number;Check(Close(t[0],.2f)&&Close(t[1],.4f),"triplanar upward projection samples XZ");
    Fixture data(NodeKind::Cavity);data.context.cavity=.73f;Check(Close(data.Value(0).number[0],.73f)&&data.program.NeedsSurfaceData(),"geometry data nodes use explicit supplied data");
    MaterialGraphDocument loaded;std::string error;texture.graph.FindNode(texture.node)->resource="Textures/stone.png";texture.graph.FindNode(texture.node)->assetGuid="stable-guid";texture.graph.FindNode(texture.node)->srgb=false;
    Check(loaded.FromJson(texture.graph.ToJson(),error)&&loaded==texture.graph,"texture path, GUID and color space survive version-2 serialization");
    auto v1=MaterialGraphDocument::Example().ToJson();const auto at=v1.find("\"version\": 2");if(at!=std::string::npos)v1.replace(at,12,"\"version\": 1");Check(loaded.FromJson(v1,error)&&loaded.nodes.size()==3,"version-1 graph still loads");
}
void LayersAndBaking() {
    MaterialGraphDocument d;const auto a=d.AddNode(NodeKind::MaterialLayer,{}),b=d.AddNode(NodeKind::MaterialLayer,{}),blend=d.AddNode(NodeKind::HeightBlend,{}),output=d.AddNode(NodeKind::PbrOutput,{});
    d.FindNode(a)->value={1,0,0,1};d.FindNode(a)->settings={.2f,0,.9f,.2f};d.FindNode(b)->value={0,1,0,1};d.FindNode(b)->settings={.8f,1,.3f,.8f};std::string error;
    d.Connect(PinId(a,8),PinId(blend,0),error);d.Connect(PinId(b,8),PinId(blend,1),error);d.Connect(PinId(blend,4),PinId(output,8),error);
    MaterialGraphProgram p;std::vector<std::array<GraphValue,14>> work;Check(p.Compile(d,error),"complete two-layer PBR graph compiles");const auto s=p.Evaluate({}, {}, {}, work);Check(Close(s.color[1],1)&&Close(s.roughness,.8f)&&Close(s.metallic,1)&&Close(s.ao,.3f),"height blend transfers all surface channels");
    d.FindNode(blend)->value[0]=0;p.Compile(d,error);const auto base=p.Evaluate({}, {}, {},work);Check(Close(base.color[0],1)&&Close(base.roughness,.2f),"height blend exact zero preserves base layer");
    BakedMaterialImages images;Check(BakeMaterialUV(p,{}, {},32,images,error)&&images.color.size()==32*32*4,"UV bake produces all material textures");Check(images.color[0]==255 && images.roughness[0]==51 && images.normal[2]==255,"baked pixels preserve base color, roughness and flat normal");
    const auto old=images.color;const auto height=d.AddNode(NodeKind::HeightMask,{});d.Connect(PinId(height,3),PinId(blend,2),error);p.Compile(d,error);Check(!BakeMaterialUV(p,{}, {},32,images,error)&&images.color==old,"world-dependent bake is rejected without replacing valid images");
    const auto before=p.GenerateHlsl();auto invalid=d;invalid.links[0].input=1234;Check(!p.Compile(invalid,error)&&p.GenerateHlsl()==before,"failed compilation preserves the last valid program");
    auto moved=d;moved.nodes[0].position={12,24};moved.nodes[0].name="Renamed";Check(SameEvaluationGraph(d,moved),"node movement and cosmetic rename do not invalidate preview");
    const auto preview=RenderMaterialPreview(p,{}, {},{});Check(preview.size()==128*128*4,"PBR sphere preview renders evaluated material");
}
void DeepGraph() {
    MaterialGraphDocument d;auto previous=d.AddNode(NodeKind::Scalar,{});d.FindNode(previous)->value[0]=1;
    for(int i=1;i<2047;++i){const auto next=d.AddNode(NodeKind::Multiply,{});d.FindNode(next)->value[1]=1;d.links.push_back({d.nextId++,PinId(previous,previous==1?0:2),PinId(next,0)});previous=next;}
    const auto output=d.AddNode(NodeKind::PbrOutput,{});d.links.push_back({d.nextId++,PinId(previous,2),PinId(output,1)});std::reverse(d.links.begin(),d.links.end());
    MaterialGraphProgram program;std::string error;Check(program.Compile(d,error)&&program.Instructions().size()==2048,"full 2048-node graph compiles with reverse-ordered links and no recursive stack growth");
    std::vector<std::array<GraphValue,14>> workspace;Check(Close(program.Evaluate({}, {}, {},workspace).roughness,1),"deep graph evaluates topologically with reusable workspace");
}
void CompileShader(const std::filesystem::path& file) {
#if defined(_WIN32) && defined(IX_GRAPH_DXC)
    const auto dxc=std::filesystem::path(IX_GRAPH_DXC).wstring(),shader=file.wstring(),dxil=(file.wstring()+L".dxil"),spv=(file.wstring()+L".spv");
    const wchar_t* argv[]={dxc.c_str(),L"-T",L"ps_6_0",L"-E",L"GraphValidationPS",L"-Fo",dxil.c_str(),shader.c_str(),nullptr};
    Check(_wspawnv(_P_WAIT,dxc.c_str(),argv)==0,"generated shader compiles to DXIL");
    const wchar_t* spirv[]={dxc.c_str(),L"-spirv",L"-T",L"ps_6_0",L"-E",L"GraphValidationPS",L"-Fo",spv.c_str(),shader.c_str(),nullptr};
    Check(_wspawnv(_P_WAIT,dxc.c_str(),spirv)==0,"generated shader compiles to SPIR-V");
#else
    (void)file;
    std::cout<<"SKIP DXC not configured: shader source is not a GPU compilation test\n";
#endif
}
void Shaders() {
    const auto root=std::filesystem::path(IX_GRAPH_SHADER_OUTPUT);std::filesystem::create_directories(root);
    const auto writePreview=[&](const MaterialGraphDocument& document,const char* filename) {
        MaterialGraphProgram program;std::string error;Check(program.Compile(document,error),"material template compiles");
        const auto pixels=RenderMaterialPreview(program,{}, {},{});std::ofstream image(root/filename,std::ios::binary);image.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
        const auto shader=root/(std::string(filename)+".hlsl");std::ofstream(shader)<<program.GenerateHlsl();CompileShader(shader);
    };
    writePreview(MaterialGraphDocument::WeatheredMetal(),"weathered-metal.rgba");
    writePreview(MaterialGraphDocument::MossyRock(),"mossy-rock.rgba");
    writePreview(MaterialGraphDocument::WeatheredStone(),"weathered-stone.rgba");
    for(const auto kind:AllNodeKinds()) {
        if (IsSubgraphNode(kind)) continue; // declaration/call nodes are tested through real expansion
        MaterialGraphDocument d;const auto id=d.AddNode(kind,{});std::string error;MaterialGraphProgram program;
        if(kind==NodeKind::TextureParameter){const auto sample=d.AddNode(NodeKind::TextureSample,{});d.Connect(PinId(id,0),PinId(sample,0),error);Check(program.Compile(d,error,PinId(sample,2)),"texture parameter compiler path");}
        else if(kind==NodeKind::PbrOutput)Check(program.Compile(d,error),"output compiler path");
        else {const auto pins=NodePins(kind);size_t slot=0;while(slot<pins.size()&&pins[slot].input)++slot;Check(program.Compile(d,error,PinId(id,slot)),NodeKindName(kind));}
        // Compile connected inputs too: this exercises every typed HLSL value and swizzle.
        const auto pins=NodePins(kind);
        for(size_t slot=0;slot<pins.size();++slot)if(pins[slot].input) {
            NodeKind sourceKind=NodeKind::Scalar;
            switch(pins[slot].type){case PinType::Color:sourceKind=NodeKind::Color;break;case PinType::Vector2:sourceKind=NodeKind::Vector2;break;case PinType::Vector3:sourceKind=NodeKind::Vector3;break;case PinType::Texture:sourceKind=NodeKind::TextureParameter;break;case PinType::Surface:sourceKind=NodeKind::MaterialLayer;break;default:break;}
            const auto source=d.AddNode(sourceKind,{});d.Connect(PinId(source,sourceKind==NodeKind::MaterialLayer?8:0),PinId(id,slot),error);
        }
        if(kind==NodeKind::PbrOutput)Check(program.Compile(d,error),"connected PBR shader compiles");
        else if(kind!=NodeKind::TextureParameter){size_t slot=0;while(slot<pins.size()&&pins[slot].input)++slot;Check(program.Compile(d,error,PinId(id,slot)),"connected typed node compiles");}
        const auto file=root/(std::to_string(static_cast<int>(kind))+".hlsl");std::ofstream(file)<<program.GenerateHlsl();
        CompileShader(file);
    }
}
}
// Time, Scene Depth and Sky Reflection read the scene a live material is drawn in; without one they
// use stand-ins. Noise Normal is the noise's slope as a tangent-space normal.
void SceneNodes() {
    Fixture time(NodeKind::Time);time.context.time=2.5f;Check(Close(time.Value(0).number[0],2.5f),"Time reads the evaluation time");
    Fixture depth(NodeKind::SceneDepth);
    Check(Close(depth.Value(0).number[0],5),"Scene Depth without a scene uses its fallback depth");
    depth.context.sceneDepth=1.25f;Check(Close(depth.Value(0).number[0],1.25f),"Scene Depth reads the scene behind the surface");
    depth.context.sceneDepth=-1;depth.graph.FindNode(depth.node)->value[0]=-3;Check(depth.Value(0).number[0]==0,"a negative fallback depth reads as no depth");
    const auto srgb=[](float a,float b,float c){return Float4{SrgbToLinear(a),SrgbToLinear(b),SrgbToLinear(c),1};};
    const auto near=[](Float4 a,Float4 b){return Close(a[0],b[0])&&Close(a[1],b[1])&&Close(a[2],b[2]);};
    Fixture sky(NodeKind::SkyReflection);sky.context.worldNormal={0,1,0,0};
    sky.context.viewDirection={0,1,0,0};Check(near(sky.Value(1).number,srgb(.22f,.42f,.78f)),"looking straight down a flat surface mirrors the zenith");
    sky.context.viewDirection={0,0,1,0};Check(near(sky.Value(1).number,srgb(.72f,.82f,.93f)),"a grazing view mirrors the horizon");
    sky.context.viewDirection={0,1,0,0};sky.graph.FindNode(sky.node)->value={.6f,0,.8f,0};
    const auto tilted=sky.Value(1).number;Check(!near(tilted,srgb(.22f,.42f,.78f)),"the tangent normal turns the mirrored direction");
    // Noise Normal: flat at strength 0, steeper with strength, computed only when read.
    Fixture noise(NodeKind::Noise);noise.context.uv={.37f,.61f,0,0};noise.graph.FindNode(noise.node)->value={9,3,.5f,4};
    const float mask=noise.Value(2).number[0];
    Check(noise.Value(4).number!=Float4{} && Close(noise.Value(2).number[0],mask),"reading Noise Normal leaves its mask unchanged");
    const auto one=noise.Value(4).number;
    noise.graph.FindNode(noise.node)->settings[1]=2;const auto two=noise.Value(4).number;
    noise.graph.FindNode(noise.node)->settings[1]=0;const auto flat=noise.Value(4).number;
    Check(Close(flat[0],0)&&Close(flat[1],0)&&Close(flat[2],1),"Noise Normal at strength 0 is flat");
    Check(one[2]>0 && std::abs(one[0])+std::abs(one[1])>1e-3f && Close(two[0]/two[2],2*one[0]/one[2]) && Close(two[1]/two[2],2*one[1]/one[2]),"Noise Normal slope scales with its strength");
    // The slope's direction: the normal leans away from where the noise rises.
    noise.graph.FindNode(noise.node)->settings[1]=1;
    const auto at=[&](float u,float v){noise.context.uv={u,v,0,0};return noise.Value(2).number[0];};
    const float rise=at(.37f+.01f/9,.61f)-at(.37f,.61f);noise.context.uv={.37f,.61f,0,0};
    Check((rise>0)==(noise.Value(4).number[0]<0),"Noise Normal leans against the rising noise");
    // Shader text: the normal only when read; every scene node compiles through DXC.
    MaterialGraphDocument d;std::string error;MaterialGraphProgram program;
    const auto n=d.AddNode(NodeKind::Noise,{}),t=d.AddNode(NodeKind::Time,{}),s=d.AddNode(NodeKind::SceneDepth,{}),r=d.AddNode(NodeKind::SkyReflection,{}),o=d.AddNode(NodeKind::PbrOutput,{});
    const auto v=d.AddNode(NodeKind::MakeVector,{});d.Connect(PinId(t,0),PinId(v,2),error);d.Connect(PinId(v,3),PinId(n,0),error);
    d.Connect(PinId(n,2),PinId(o,1),error);d.Connect(PinId(s,0),PinId(o,7),error);d.Connect(PinId(r,1),PinId(o,3),error);
    Check(program.Compile(d,error) && program.GenerateHlsl().find("GraphNoiseNormal(n")==std::string::npos,"an unread Noise Normal is not computed");
    d.Connect(PinId(n,4),PinId(o,4),error);d.Connect(PinId(n,4),PinId(r,0),error);
    Check(program.Compile(d,error) && program.GenerateHlsl().find("GraphNoiseNormal(n")!=std::string::npos,"a read Noise Normal is computed");
    MaterialGraphDocument loaded;Check(loaded.FromJson(d.ToJson(),error) && loaded==d,"scene nodes and the Noise Normal link survive saving");
    Check(program.Layout().worldDependent,"Sky Reflection makes a material view dependent");
    const auto root=std::filesystem::path(IX_GRAPH_SHADER_OUTPUT);std::filesystem::create_directories(root);
    const auto file=root/"scene-nodes.hlsl";std::ofstream(file)<<program.GenerateHlsl();CompileShader(file);
}
int main(){Numbers();SpatialAndTextures();LayersAndBaking();DeepGraph();Shaders();SceneNodes();std::cout<<"Material graph compiler failures: "<<failures<<'\n';return failures?1:0;}
