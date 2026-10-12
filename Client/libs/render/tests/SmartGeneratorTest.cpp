#include "tools/material/SmartGenerator.h"
#include "tools/material/MaterialInstance.h"
#include "tools/material/MaterialGraphImages.h"
#include "GeometryFixtures.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#include <process.h>
#endif
using namespace material_tool;
namespace {
int failures=0;
constexpr const char* Guid="f4444444-4444-4444-8444-444444444444";
void Check(bool ok,const std::string& message){if(!ok){std::cerr<<"FAIL "<<message<<'\n';++failures;}}
bool Near(float a,float b){return std::abs(a-b)<.0001f;}
struct Fixture {
    SmartGenerator generator;
    MaterialGraphDocument graph=MaterialGraphDocument::Example();
    MaterialGraphProgram program;
    ParameterOverrides values;
    std::string error,prefix;
    std::uint32_t call=0;
    MaterialSubgraphResolver Resolver(){return [&](const auto& guid,auto& result,auto& message){if(guid!=Guid){message="Missing generator";return false;}result=generator.definition;return true;};}
    explicit Fixture(SmartGeneratorKind kind){
        if(!SmartGenerator::Create(kind,generator,error))throw std::runtime_error("create recipe: "+error);
        call=AddSmartGenerator(graph,Guid,generator,0,error);Check(call!=0,"generator inserts as ordinary subgraph");
        Check(graph.Connect(PinId(call,1),PinId(3,1),error),"scalar mask can feed an ordinary node");
        Check(program.Compile(graph,error,PinId(call,1),Resolver()),"compile shared IR: "+error);prefix="Subgraph_"+std::to_string(call)+"/";Set("NoiseInfluence",0);
    }
    void Set(const char* name,float value){values.values[prefix+name]={value,0,0,0};}
    void Direction(const char* name,Float4 value){values.values[prefix+name]=value;}
    float Mask(EvaluationContext c={}){std::vector<std::array<GraphValue,14>> workspace;program.Evaluate(c,values,{},workspace);return program.PreviewValue(workspace).number[0];}
};
void References(){
    EvaluationContext c;c.curvature=.625f;c.cavity=.4f;c.ao=.8f;
    Fixture edge(SmartGeneratorKind::EdgeWear);Check(Near(edge.Mask(c),.5f),"wear curvature transition reference");edge.Set("WearAmount",.4f);Check(Near(edge.Mask(c),.2f),"wear amount");edge.Set("WearAmount",1);edge.Set("Contrast",2);Check(Near(edge.Mask(c),.25f),"contrast power reference");c.curvature=.5f;Check(Near(edge.Mask(c),0),"flat surface has no convex wear");c.curvature=.25f;Check(Near(edge.Mask(c),0),"concave surface has no convex wear");
    Fixture dirt(SmartGeneratorKind::DirtAccumulation);Check(Near(dirt.Mask(c),.4f),"dirt cavity reference");c.cavity=0;c.ao=.1f;Check(Near(dirt.Mask(c),.9f),"dirt sheltered AO reference");dirt.Set("DirtAmount",0);Check(Near(dirt.Mask(c),0),"zero dirt amount");
    c.curvature=.75f;c.cavity=.2f;
    Fixture rust(SmartGeneratorKind::RustSpread);Check(Near(rust.Mask(c),.4f),"rust independent edge/cavity weights");rust.Set("RustAmount",.5f);Check(Near(rust.Mask(c),.2f),"rust amount reference");rust.Set("EdgeInfluence",0);Check(Near(rust.Mask(c),.075f),"rust cavity influence remains independent");
    c={};Fixture moss(SmartGeneratorKind::MossGrowth);Check(Near(moss.Mask(c),1),"moss up-facing growth");c.worldNormal={0,-1,0,0};Check(Near(moss.Mask(c),0),"moss reverse-facing rejection");c.worldNormal={1,0,0,0};moss.Direction("GrowthDirection",{1,0,0,0});Check(Near(moss.Mask(c),1),"growth direction changes facing mask");moss.Direction("GrowthDirection",{0,0,0,0});c.worldNormal={0,1,0,0};Check(Near(moss.Mask(c),1),"zero growth direction uses documented up fallback");moss.Set("HeightInfluence",1);c.worldPosition={0,5,0,0};Check(Near(moss.Mask(c),.5f),"moss projected-height reference");c.worldPosition[1]=10;Check(Near(moss.Mask(c),0),"moss height cutoff");
    c={};Fixture dust(SmartGeneratorKind::DustAccumulation);Check(Near(dust.Mask(c),.75f),"dust exposed up surface reference");c.ao=0;Check(Near(dust.Mask(c),1),"dust sheltered up surface reference");c.worldNormal={1,0,0,0};Check(Near(dust.Mask(c),0),"dust wall rejection");
    Fixture water(SmartGeneratorKind::WaterStreaks);Check(Near(water.Mask(),1),"disabled water breakup gives full coverage");water.Set("StreakAmount",0);Check(Near(water.Mask(),0),"zero streak amount");water.Set("StreakAmount",1);water.Set("NoiseInfluence",1);
    const auto variation=[&](int axis){float sum=0;for(int i=0;i<128;++i){EvaluationContext p;p.worldPosition={i*.071f+.34f,i*.113f-.2f,i*.031f+.8f,0};const auto a=water.Mask(p);p.worldPosition[axis]+=.02f;sum+=std::abs(a-water.Mask(p));}return sum;};
    Check(variation(0)>3*variation(1),"water pattern elongates along default gravity");water.Direction("GravityDirection",{1,0,0,0});Check(variation(1)>3*variation(0),"water pattern follows rotated gravity");water.Direction("GravityDirection",{0,0,0,0});Check(variation(0)>3*variation(1),"zero gravity falls back down");water.Set("StreakLength",0);water.Set("StreakWidth",-1);Check(std::isfinite(water.Mask()),"zero/negative streak sizes are bounded");
}
void AssetsLayersAndShaders(){
    GeometryBake bake;GeometryBakeSettings settings;settings.resolution=32;std::string error;
    Check(BakeGeometry(geometry_fixtures::Corner(),settings,bake,error),"real generator geometry fixture");
    for(const auto& preset:SmartGeneratorPresets()){
        Fixture f(preset.kind);SmartGenerator restored;
        Check(restored.FromJson(f.generator.ToJson(),error) && restored==f.generator,"generator wrapper exact roundtrip");const auto intact=restored;
        Check(!restored.FromJson("broken",error) && restored==intact,"invalid generator is transactional");
        auto invalid=f.generator;invalid.name.clear();Check(!restored.FromJson(invalid.ToJson(),error) && restored==intact,"invalid metadata rejected");
        invalid=f.generator;for(auto& n:invalid.definition.graph.nodes)if(n.kind==NodeKind::SubgraphOutput)n.parameter="Other";
        Check(!invalid.Validate(error),"generator enforces one scalar Mask interface");
        Check(f.program.Layout().worldDependent && f.program.NeedsSurfaceData()==(preset.kind!=SmartGeneratorKind::WaterStreaks),"real geometry/context requirements survive expansion");
        f.Set("NoiseInfluence",1);bool changed=false;
        for(int i=0;i<64;++i){EvaluationContext c;c.worldPosition={i*.117f,.34f+i*.017f,.73f,0};c.curvature=.75f;c.cavity=.4f;c.ao=.4f;f.Set("Seed",19);const auto a=f.Mask(c);Check(a>=0 && a<=1 && Near(a,f.Mask(c)),"bounded deterministic generator output");f.Set("Seed",23);changed|=std::abs(a-f.Mask(c))>.0001f;}
        Check(changed,"seed changes each recipe's procedural output");
        f.Set("Scale",1000000);f.Set("Seed",1000000);EvaluationContext distant;distant.worldPosition={1000000,-1000000,1000000,0};distant.curvature=.75f;distant.cavity=.5f;Check(std::isfinite(f.Mask(distant)),"large allowed inputs stay finite before noise lattice conversion");f.Set("Scale",3);f.Set("Seed",19);
        MaterialGraphProgram standalone;const auto output=std::find_if(f.generator.definition.graph.nodes.begin(),f.generator.definition.graph.nodes.end(),[](const auto& n){return n.kind==NodeKind::SubgraphOutput;})->id;
        Check(standalone.Compile(f.generator.definition.graph,error,PinId(output,1)),"standalone generator authoring uses existing preview compiler");
        PreviewSettings preview;preview.channel=PreviewChannel::SelectedOutput;Check(RenderMaterialPreview(f.program,f.values,{},preview,32,&bake).size()==4096,"real mesh generator mask preview");
        BakedMaterialImages images;Check(BakeMaterialOnGeometry(f.program,f.values,{},bake,images,error),"generator material bakes on real geometry");
        const auto shader=f.program.GenerateHlsl();std::reverse(f.graph.nodes.begin(),f.graph.nodes.end());std::reverse(f.graph.links.begin(),f.graph.links.end());Check(f.program.Compile(f.graph,error,PinId(f.call,1),f.Resolver()) && f.program.GenerateHlsl()==shader,"generator HLSL stable under storage reordering");
        const auto source=std::filesystem::path(IX_GENERATOR_SHADER_OUTPUT)/(std::string(preset.id)+".hlsl");std::filesystem::create_directories(source.parent_path());std::ofstream(source)<<shader;
#if defined(_WIN32) && defined(IX_GRAPH_DXC)
        const auto dxc=std::filesystem::path(IX_GRAPH_DXC).wstring(),file=source.wstring(),dxil=file+L".dxil",spv=file+L".spv";
        const wchar_t* args[]={dxc.c_str(),L"-T",L"ps_6_0",L"-E",L"GraphValidationPS",L"-Fo",dxil.c_str(),file.c_str(),nullptr};Check(_wspawnv(_P_WAIT,dxc.c_str(),args)==0,"generator compiles to DXIL");
        const wchar_t* spirv[]={dxc.c_str(),L"-spirv",L"-T",L"ps_6_0",L"-E",L"GraphValidationPS",L"-Fo",spv.c_str(),file.c_str(),nullptr};Check(_wspawnv(_P_WAIT,dxc.c_str(),spirv)==0,"generator compiles to SPIR-V");
#else
        std::cout<<"SKIP DXC not configured; no GPU compilation claim\n";
#endif
    }
    Fixture f(SmartGeneratorKind::EdgeWear);auto graph=MaterialGraphDocument::WeatheredStone();const auto layer=graph.layers.back().id;const auto call=AddSmartGenerator(graph,Guid,f.generator,layer,error);
    Check(call && graph.FindLayer(layer)->maskPin==PinId(call,1) && graph.ValidateLayerStack(error),"generator output assigns to canonical managed layer mask");
    const auto second=AddSmartGenerator(graph,Guid,f.generator,0,error);MaterialGraphProgram program;
    Check(program.Compile(graph,error,PinId(second,1),f.Resolver()),"multiple generator calls share definition with independent scopes");
    ParameterOverrides overrides;overrides.values["Subgraph_"+std::to_string(call)+"/WearAmount"]={.2f,0,0,0};overrides.values["Subgraph_"+std::to_string(second)+"/WearAmount"]={.8f,0,0,0};Check(program.ValidateOverrides(overrides,error),"generator instance scopes are independent");
    auto shared=std::make_shared<MaterialGraphProgram>(program);MaterialInstance instance;instance.definitionGuid=Guid;instance.overrides=overrides;MaterialInstanceBinding binding;
    Check(binding.Bind(Guid,shared,instance,error),"instances share expanded immutable generator program");
    const auto before=graph;Check(!AddSmartGenerator(graph,Guid,f.generator,999999,error) && graph==before,"bad layer assignment preserves graph");Check(!AddSmartGenerator(graph,"missing",f.generator,0,error) && graph==before,"bad asset GUID preserves graph");
    SmartGenerator generator=f.generator;Check(!SmartGenerator::Create(static_cast<SmartGeneratorKind>(999),generator,error) && generator==f.generator,"unknown built-in cannot create a placeholder");
}
void Benchmark(){
    std::string error;GeometryBake geometry;GeometryBakeSettings settings;settings.resolution=128;Check(BakeGeometry(geometry_fixtures::Corner(),settings,geometry,error),"benchmark geometry");
    for(const auto& preset:SmartGeneratorPresets()){
        Fixture f(preset.kind);f.Set("NoiseInfluence",.5f);
        const auto measure=[&](const auto& task){std::vector<double> samples;for(int i=0;i<7;++i){const auto start=std::chrono::steady_clock::now();task();if(i>=2)samples.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}std::sort(samples.begin(),samples.end());return samples[2];};
        const auto compile=measure([&]{Check(f.program.Compile(f.graph,error,PinId(f.call,1),f.Resolver()),"benchmark compile");});
        PreviewSettings preview;preview.channel=PreviewChannel::SelectedOutput;
        const auto render=measure([&]{auto pixels=RenderMaterialPreview(f.program,f.values,{},preview,128,&geometry);Check(pixels.size()==128*128*4,"benchmark preview");});
        std::cout<<preset.name<<": compile="<<compile<<" ms, preview128="<<render<<" ms, IR="<<f.program.Instructions().size()<<", params="<<f.program.Layout().parameterBufferBytes<<" bytes\n";
    }
}
}
int main(int argc,char** argv){try{if(argc>1 && std::string(argv[1])=="--benchmark")Benchmark();else{References();AssetsLayersAndShaders();}}catch(const std::exception& e){Check(false,e.what());}std::cout<<"Smart Generator failures: "<<failures<<'\n';return failures?1:0;}
