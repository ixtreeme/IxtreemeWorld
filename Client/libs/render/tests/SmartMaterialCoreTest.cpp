#include "tools/material/MaterialInstance.h"
#include "tools/material/MaterialGraphImages.h"
#include "tools/material/MaterialGraphIO.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

using namespace material_tool;
namespace
{
int failures = 0;
void Check(bool condition, const char* message)
{ std::cout << (condition ? "PASS " : "FAIL ") << message << '\n'; failures += !condition; }
bool Close(float a, float b) { return std::abs(a - b) < 0.0001f; }
constexpr const char* Guid = "e8013a02-8d6a-4bcd-9aba-bc1000000001";
Surface Evaluate(const MaterialDefinition& d)
{
    MaterialGraphProgram program; std::string error; std::vector<std::array<GraphValue, 14>> workspace;
    Check(program.Compile(d, error), "canonical layer definition compiles through existing IR");
    if (!error.empty()) std::cout << error << '\n';
    return program.Evaluate({}, {}, {}, workspace);
}
void Layers()
{
    MaterialDefinition d; std::string error;
    const auto base = d.AddMaterialLayer("Base", error), top = d.AddMaterialLayer("Top", error);
    Check(base && top && base != top && d.layers.size() == 2, "managed layers receive stable unique IDs");
    if (!base || !top) { std::cout << error << '\n'; return; }
    d.FindNode(d.FindLayer(base)->sourceNode)->value = {1, 0, 0, 1};
    d.FindNode(d.FindLayer(base)->sourceNode)->settings[0] = .2f;
    d.FindNode(d.FindLayer(top)->sourceNode)->value = {0, 1, 0, 1};
    d.FindNode(d.FindLayer(top)->sourceNode)->settings[0] = .8f;
    d.FindNode(d.FindLayer(top)->opacityNode)->value[0] = .25f;
    auto s = Evaluate(d);
    Check(Close(s.color[0], .75f) && Close(s.color[1], .25f) && Close(s.roughness, .35f), "opacity uses the same canonical parameter node in graph and stack");
    const auto blend = d.FindLayer(top)->blendNode;
    d.FindNode(blend)->settings[2] = 2;
    s = Evaluate(d);
    Check(s.color[0] == 1 && Close(s.roughness, .35f), "per-channel override can change roughness while preserving color");
    MaterialDefinition loaded;
    Check(loaded.FromJson(d.ToJson(), error) && loaded == d, "v3 round-trip preserves layers, ownership, values and ID allocator");
    const auto intact = d;
    Check(!d.Connect(PinId(d.FindLayer(base)->sourceNode, 8), PinId(blend, 0), error) && d == intact, "managed topology rejects Node Editor rewiring without mutation");
    Check(!d.RemoveNode(blend) && !d.RemoveLink(d.links.front().id), "managed nodes and links reject generic deletion");
    auto broken = d; broken.FindNode(blend)->owner = 999;
    Check(!loaded.FromJson(broken.ToJson(), error) && loaded == intact, "orphan layer ownership fails transactionally");
    broken = d; broken.layers[0].id = broken.layers[1].id;
    Check(!loaded.FromJson(broken.ToJson(), error) && loaded == intact, "duplicate layer IDs fail without replacing current definition");
    broken = d; std::erase_if(broken.links, [&](const auto& l) { return l.input == PinId(blend, 0); });
    Check(!loaded.FromJson(broken.ToJson(), error), "desynchronized layer chain is diagnosed");
    Check(!d.MoveMaterialLayer(top, 999, error) && d == intact, "invalid reorder preserves all graph data");
    Check(!d.SetMaterialLayerMask(top, PinId(d.FindLayer(base)->sourceNode, 8), error) && d == intact, "surface output cannot be used as a scalar mask");
    Check(d.SetMaterialLayerEnabled(top, false, error) && Close(Evaluate(d).roughness, .2f), "disabled layer is removed from evaluation chain");
    Check(d.SetMaterialLayerEnabled(top, true, error), "reenabling layer restores its chain");
    Check(d.MoveMaterialLayer(base, 1, error) && Close(Evaluate(d).roughness, .2f), "reorder changes composition and retains stable node identities");
    Check(d.FindLayer(top)->blendNode == blend, "reordering preserves layer node IDs");
    const auto copy = d.DuplicateMaterialLayer(top, error);
    Check(copy && copy != top && d.FindNode(d.FindLayer(copy)->opacityNode)->value[0] == .25f, "duplication gives fresh IDs and preserves canonical defaults");
    Check(d.SetMaterialLayerHeightBlend(copy, true, error) && d.ValidateLayerStack(error), "height blend updates typed surface links consistently");
    Check(d.RemoveMaterialLayer(copy, error) && d.layers.size() == 2, "layer removal rewires the remaining chain");
    const auto mask = d.AddNode(NodeKind::Scalar, {}); d.FindNode(mask)->value[0] = 0;
    Check(d.SetMaterialLayerMask(base, PinId(mask, 0), error) && !d.RemoveNode(mask), "assigned custom masks cannot be silently deleted");
    MaterialGraphProgram program; Check(program.Compile(d, error), "masked layer graph compiles"); const auto source = program.GenerateHlsl();
    const auto before = d; d.ConvertToCustomGraph();
    Check(d.layers.empty() && program.Compile(d, error) && program.GenerateHlsl() == source, "explicit conversion retains channel semantics and generated shader");
    Check(d.ToJson().find("\"version\": 3")!=std::string::npos,"detached channel overrides remain versioned so older compilers cannot silently misinterpret them");
    d = before;
    Check(d.layers.size() == 2 && d.ValidateLayerStack(error), "snapshot restoration makes conversion undoable");
    auto custom = MaterialGraphDocument::Example(); const auto saved = custom;
    Check(!custom.AddMaterialLayer("Unsafe conversion", error) && custom == saved, "existing custom material is never implicitly converted");
    MaterialDefinition emptyStack;
    const auto only = emptyStack.AddMaterialLayer("Only", error);
    Check(emptyStack.RemoveMaterialLayer(only, error) && emptyStack.layers.empty() && emptyStack.HasLayerStack(), "removing the last layer retains the managed output and editable stack");
    Check(loaded.FromJson(emptyStack.ToJson(), error) && loaded == emptyStack && Evaluate(emptyStack).opacity == 1, "empty managed stack round-trips and evaluates a safe default surface");
    const auto newLayer = emptyStack.AddMaterialLayer("New", error);
    Check(newLayer > only && emptyStack.ValidateLayerStack(error), "an emptied stack accepts new layers with fresh stable IDs");
}
void InstancesAndLayout()
{
    auto d = MaterialGraphDocument::Example(); std::string error;
    const auto scalar = d.AddNode(NodeKind::ScalarParameter, {}), color = d.AddNode(NodeKind::ColorParameter, {}), texture = d.AddNode(NodeKind::TextureParameter, {});
    d.FindNode(scalar)->parameter = "Roughness"; d.FindNode(color)->parameter = "Color"; d.FindNode(texture)->parameter = "Albedo";
    d.Connect(PinId(scalar, 0), PinId(3, 1), error); d.Connect(PinId(color, 0), PinId(3, 0), error);
    auto program = std::make_shared<MaterialGraphProgram>(); Check(program->Compile(d, error), "parameter definition compiles");
    Check(program->Layout().parameterBufferBytes == 32 && program->Layout().parameters.size() == 3, "layout packs numeric fields at float4 alignment and keeps texture type");
    MaterialInstance instance; instance.definitionGuid = Guid; instance.overrides.values["Roughness"] = {.87f, 0, 0, 0};
    instance.overrides.textures["Albedo"] = std::string("guid:") + Guid;
    MaterialInstance loaded;
    Check(loaded.FromJson(instance.ToJson(), error) && loaded == instance, "standalone instance round-trip preserves GUID references and only overrides");
    const auto intact = loaded;
    Check(!loaded.FromJson("{bad", error) && loaded == intact, "malformed instance preserves previous asset");
    auto invalid = instance; invalid.definitionGuid = "missing";
    Check(!loaded.FromJson(invalid.ToJson(), error) && loaded == intact, "invalid definition GUID is rejected");
    invalid = instance; invalid.overrides.values["Albedo"] = {1, 0, 0, 0};
    Check(!invalid.Validate(*program, error), "wrong-type texture override is diagnosed");
    invalid = instance; invalid.overrides.values["DeletedParameter"] = {1, 0, 0, 0};
    Check(!invalid.Validate(*program, error), "missing parameter override is diagnosed");
    MaterialInstanceBinding a, b;
    Check(a.Bind(Guid, program, instance, error) && b.Bind(Guid, program, instance, error) && a.Program() == b.Program(), "instances share one immutable program without cloning graphs");
    const auto shader = program->GenerateHlsl();
    auto overrides = instance.overrides; overrides.values["Roughness"][0] = .12f;
    Check(a.Update(overrides, error) && Close(a.ParameterBuffer()[0][0], .12f) && program->GenerateHlsl() == shader, "dynamic override repacks parameters without shader recompilation");
    Check(Close(b.ParameterBuffer()[0][0], .87f), "one instance update leaves another instance intact");
    overrides.values["Roughness"][0] = std::numeric_limits<float>::infinity(); const auto buffer = a.ParameterBuffer();
    Check(!a.Update(overrides, error) && a.ParameterBuffer() == buffer, "nonfinite update keeps the previous valid instance buffer");
    Check(!a.Bind("wrong-guid", program, instance, error) && a.Program() == program, "failed definition resolution preserves valid binding");
    Check(!a.Bind(Guid, {}, instance, error), "missing material program is diagnosed");
    Check(!a.Bind(Guid, std::make_shared<MaterialGraphProgram>(), instance, error), "uncompiled definition cannot masquerade as a valid material binding");
    std::reverse(d.nodes.begin(), d.nodes.end()); std::reverse(d.links.begin(), d.links.end());
    MaterialGraphProgram reordered; Check(reordered.Compile(d, error) && reordered.GenerateHlsl() == shader, "IR and parameter layout are deterministic regardless of serialized array order");
}
void Files()
{
    const auto folder = std::filesystem::temp_directory_path() / ("ix-smart-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder); const auto path = folder / "material.materialgraph";
    std::string error, text;
    Check(SaveMaterialTextAtomic(path, "original", error) && LoadMaterialText(path, text, error) && text == "original", "shared material IO writes and reads real files");
    Check(SaveMaterialTextAtomic(path, "replacement", error) && LoadMaterialText(path, text, error) && text == "replacement", "atomic save replaces an existing file");
    Check(!SaveMaterialTextAtomic(path, std::string(4 * 1024 * 1024 + 1, 'x'), error) && LoadMaterialText(path, text, error) && text == "replacement", "oversized write preserves last valid file");
    Check(!SaveMaterialTextAtomic(folder, "cannot replace directory", error) && std::filesystem::is_directory(folder), "failed replacement preserves its destination");
    const auto previous = text;
    Check(!LoadMaterialText(folder / "missing", text, error) && text == previous, "missing file load preserves previous text");
    Check(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{}) == 1, "atomic save removes temporary files after success and failure");
    std::filesystem::remove_all(folder);
}
void FoundationNodes()
{
    const auto checkNode = [&](NodeKind kind, Float4 value, int slot, Float4 expected, int components) {
        MaterialGraphDocument d; const auto id=d.AddNode(kind,{});d.FindNode(id)->value=value;
        MaterialGraphProgram p;std::string error;std::vector<std::array<GraphValue,14>> workspace;
        Check(p.Compile(d,error,PinId(id,slot)),NodeKindName(kind));p.Evaluate({}, {}, {},workspace);
        const auto actual=p.PreviewValue(workspace).number;bool match=true;
        for (int k=0;k<components;++k)match &= Close(actual[k],expected[k]);
        Check(match,"foundation node matches reference value");
        d.FindNode(id)->value[0]=std::numeric_limits<float>::infinity();const auto source=p.GenerateHlsl();
        Check(!p.Compile(d,error,PinId(id,slot)) && p.GenerateHlsl()==source,"invalid node data preserves previous compiled IR");
    };
    checkNode(NodeKind::Normalize,{3,4,0,0},1,{.6f,.8f,0,0},3);
    checkNode(NodeKind::NormalFlipY,{.6f,.8f,0,0},1,{.6f,-.8f,0,0},3);
    checkNode(NodeKind::NormalUnpack,{.5f,.5f,1,1},1,{0,0,1,0},3);
    checkNode(NodeKind::NormalStrength,{1,0,0,0},2,{0,0,1,0},3);
    checkNode(NodeKind::Cross,{1,0,0,0},2,{0,0,1,0},3);
    checkNode(NodeKind::Length,{3,4,0,0},1,{5,0,0,0},1);
    checkNode(NodeKind::Distance,{3,5,0,0},2,{5,0,0,0},1);
    checkNode(NodeKind::Reflect,{1,-1,0,0},2,{1,1,0,0},3);
    checkNode(NodeKind::Sine,{-1.25f,0,0,0},1,{std::sin(-1.25f),0,0,0},1);
    checkNode(NodeKind::Cosine,{-1.25f,0,0,0},1,{std::cos(-1.25f),0,0,0},1);
    checkNode(NodeKind::Fract,{-1.25f,0,0,0},1,{.75f,0,0,0},1);
    checkNode(NodeKind::Floor,{-1.25f,0,0,0},1,{-2,0,0,0},1);
    checkNode(NodeKind::Ceil,{-1.25f,0,0,0},1,{-1,0,0,0},1);
    checkNode(NodeKind::Sign,{-1.25f,0,0,0},1,{-1,0,0,0},1);
    checkNode(NodeKind::Step,{.2f,.3f,0,0},2,{1,0,0,0},1);
    checkNode(NodeKind::MaskCombine,{.2f,.3f,0,0},2,{.06f,0,0,0},1);
    checkNode(NodeKind::MaskLevels,{.25f,0,1,2},4,{.5f,0,0,0},1);
    checkNode(NodeKind::MaskContrast,{.75f,2,0,0},2,{1,0,0,0},1);
    checkNode(NodeKind::MaskInvert,{-1.25f,0,0,0},1,{1,0,0,0},1);
    checkNode(NodeKind::Fresnel,{5,0,0,0},3,{1,0,0,0},1);
    checkNode(NodeKind::Vector2Parameter,{2,3,0,0},0,{2,3,0,0},2);
    checkNode(NodeKind::Vector3Parameter,{2,3,4,0},0,{2,3,4,0},3);
    MaterialDefinition normalGraph;const auto input=normalGraph.AddNode(NodeKind::Vector3,{}),strength=normalGraph.AddNode(NodeKind::NormalStrength,{});
    normalGraph.FindNode(input)->value={.6f,0,.8f,0};std::string error;normalGraph.Connect(PinId(input,0),PinId(strength,0),error);
    MaterialGraphProgram p;std::vector<std::array<GraphValue,14>> workspace;
    for (float amount:{0.0f,.5f,1.0f,2.0f}) {
        normalGraph.FindNode(strength)->value[0]=amount;p.Compile(normalGraph,error,PinId(strength,2));p.Evaluate({}, {}, {},workspace);
        const auto n=p.PreviewValue(workspace).number;const auto expected=amount==0?Float4{0,0,1,0}:Normalize3({.6f*amount,0,.8f,0});
        Check(Close(n[0],expected[0]) && Close(n[2],expected[2]),"normal strength supports zero, attenuation, identity and amplification");
    }
    normalGraph.FindNode(input)->value={0,0,0,0};p.Compile(normalGraph,error,PinId(strength,2));p.Evaluate({}, {}, {},workspace);
    Check(p.PreviewValue(workspace).number==Float4{0,0,1,0},"zero-length normal has stable flat fallback");
    MaterialDefinition masks;const auto mask=masks.AddNode(NodeKind::MaskCombine,{});masks.FindNode(mask)->settings[0]=9;
    Check(!p.Compile(masks,error,PinId(mask,2)),"unsupported mask combine mode reports a compilation diagnostic");
    const auto levels=masks.AddNode(NodeKind::MaskLevels,{});masks.FindNode(mask)->settings[0]=0;masks.FindNode(levels)->value={.5f,.5f,.5f,0};
    Check(p.Compile(masks,error,PinId(levels,4)),"degenerate levels compile");p.Evaluate({}, {}, {},workspace);
    Check(p.PreviewValue(workspace).number[0]==0,"zero levels range and zero gamma stay finite");
}
void Stone()
{
    auto d=MaterialGraphDocument::WeatheredStone();MaterialGraphProgram p;std::string error;
    Check(d.layers.size()==3 && p.Compile(d,error) && p.NeedsSurfaceData() && !p.CanBakeUV(),"Weathered Stone is one three-layer graph with explicit geometry requirements");
    if (!error.empty()) std::cout<<error<<'\n';
    for (const char* name:{"StoneTiling","StoneNormalStrength","DirtAmount","DirtContrast","MossAmount","MossScale","MossSlopeThreshold","MossSeed"})
        Check(std::any_of(p.Layout().parameters.begin(),p.Layout().parameters.end(),[&](const auto& parameter){return parameter.name==name;}),"Weathered Stone publishes requested parameter");
    MaterialDefinition loaded;Check(loaded.FromJson(d.ToJson(),error) && loaded==d,"Weathered Stone round-trips managed and custom regions");
    std::vector<std::array<GraphValue,14>> workspace;EvaluationContext context;context.worldPosition={.31f,.73f,.29f,0};
    ParameterOverrides overrides;overrides.values["MossAmount"]={0,0,0,0};overrides.values["DirtAmount"]={0,0,0,0};
    auto s=p.Evaluate(context,overrides,{},workspace);Check(Close(s.normal[2],1) && Close(s.normal[0],0),"unassigned optional normal texture uses encoded flat fallback");
    const auto source=p.GenerateHlsl();overrides.values["MossAmount"]={1,0,0,0};
    const auto up=p.Evaluate(context,overrides,{},workspace);context.worldNormal={1,0,0,0};const auto side=p.Evaluate(context,overrides,{},workspace);
    Check(up.color!=side.color && source==p.GenerateHlsl(),"geometric normal affects moss with no parameter recompilation");
    context.cavity=.8f;context.ao=.2f;overrides.values["MossAmount"]={0,0,0,0};overrides.values["DirtAmount"]={1,0,0,0};
    const auto recess=p.Evaluate(context,overrides,{},workspace);Check(recess.color!=s.color,"explicit cavity and AO test inputs affect dirt");
    Check(recess.color==p.Evaluate(context,overrides,{},workspace).color,"procedural material evaluation is deterministic");
    BakedMaterialImages images;Check(!BakeMaterialUV(p,overrides,{},16,images,error) && !error.empty(),"Weathered Stone cannot silently export fake geometry through UV-only baker");
    Check(RenderMaterialPreview(p,overrides,{},{}).size()==128*128*4,"Weathered Stone renders through existing CPU preview");
}
void Benchmark()
{
    using Clock=std::chrono::steady_clock;
    const auto median=[&](auto operation) {
        operation();std::vector<double> samples;
        for (int run=0;run<5;++run) {const auto start=Clock::now();operation();samples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());}
        std::sort(samples.begin(),samples.end());return samples[2];
    };
    const auto d=MaterialGraphDocument::WeatheredStone();MaterialGraphProgram p;std::string error;
    const double compile=median([&]{if(!p.Compile(d,error))++failures;});
    std::size_t bytes=0;const double hlsl=median([&]{bytes=p.GenerateHlsl().size();});
    PreviewSettings settings;
    const double preview128=median([&]{bytes=RenderMaterialPreview(p,{}, {},settings,128).size();});
    const double preview256=median([&]{bytes=RenderMaterialPreview(p,{}, {},settings,256).size();});
    auto program=std::make_shared<MaterialGraphProgram>(p);MaterialInstance instance;instance.definitionGuid=Guid;
    MaterialInstanceBinding binding;binding.Bind(Guid,program,instance,error);
    ParameterOverrides overrides;overrides.values["MossAmount"]={.37f,0,0,0};
    const double updates=median([&]{for(int run=0;run<1000;++run)if(!binding.Update(overrides,error))++failures;});
    MaterialGraphProgram uv;uv.Compile(MaterialGraphDocument::WeatheredMetal(),error);BakedMaterialImages images;
    const double bake=median([&]{if(!BakeMaterialUV(uv,{}, {},128,images,error))++failures;});
    std::cout<<"PROFILE median_of_5_after_warmup\n"
        <<"weathered_stone_compile_ms="<<compile<<"\nhlsl_generation_ms="<<hlsl
        <<"\nweathered_stone_preview_128_ms="<<preview128<<"\nweathered_stone_preview_256_ms="<<preview256
        <<"\ninstance_1000_updates_ms="<<updates<<"\nweathered_metal_uv_bake_128_ms="<<bake
        <<"\nstone_nodes="<<d.nodes.size()<<"\nstone_reachable_instructions="<<p.Instructions().size()
        <<"\nstone_parameter_buffer_bytes="<<binding.ParameterBuffer().size()*sizeof(Float4)
        <<"\nstone_declared_texture_bindings="<<p.Layout().textures.size()
        <<"\ninstance_binding_header_bytes="<<sizeof(MaterialInstanceBinding)<<"\nlast_preview_bytes="<<bytes<<'\n';
    std::cout<<"CPU reference only; optional textures use neutral constants; geometry fields are explicit test inputs. No GPU performance or geometry-bake claim.\n";
}
}
int main(int argc,char** argv) { if(argc>1 && std::string(argv[1])=="--benchmark")Benchmark();else {Layers(); InstancesAndLayout(); Files(); FoundationNodes(); Stone();} std::cout << "Smart material core failures: " << failures << '\n'; return failures ? 1 : 0; }
