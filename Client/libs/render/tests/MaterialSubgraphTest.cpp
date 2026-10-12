#include "tools/material/MaterialSubgraph.h"
#include "tools/material/MaterialInstance.h"
#include "tools/material/MaterialGraphImages.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#if defined(_WIN32)
#include <process.h>
#endif
using namespace material_tool;
namespace
{
int failures = 0;
constexpr const char* A = "f1111111-1111-4111-8111-111111111111";
constexpr const char* B = "f2222222-2222-4222-8222-222222222222";
void Check(bool ok, const char* message) { std::cout << (ok ? "PASS " : "FAIL ") << message << '\n'; failures += !ok; }
bool Close(float a, float b) { return std::abs(a - b) < .0001f; }
struct Fixture
{
    std::map<std::string, MaterialSubgraph> assets{{A, MaterialSubgraph::Example()}};
    MaterialGraphDocument graph = MaterialGraphDocument::Example();
    std::string error;
    std::uint32_t call = graph.AddNode(NodeKind::Subgraph, {});
    MaterialSubgraphResolver Resolver() { return [&](const auto& guid, auto& asset, auto& message) {
        if (!assets.contains(guid)) { message = "Missing test subgraph " + guid; return false; }
        asset = assets.at(guid); return true;
    }; }
    Fixture() {
        Check(BindMaterialSubgraph(graph, call, A, assets.at(A), error), "subgraph binds a typed stable-ID interface");
        Check(graph.Connect(PinId(call, 1), PinId(3, 1), error), "typed subgraph scalar output connects to the existing PBR graph");
    }
};
void RoundTripsAndEvaluation()
{
    Fixture f; MaterialSubgraph asset;
    Check(asset.FromJson(f.assets.at(A).ToJson(), f.error) && asset == f.assets.at(A), "subgraph asset v1 round-trip preserves the canonical graph");
    const auto intactAsset = asset;
    Check(!asset.FromJson("{broken", f.error) && asset == intactAsset, "malformed subgraph preserves the previous asset");
    auto invalid = asset; invalid.interfaceVersion = 0;
    Check(!asset.FromJson(invalid.ToJson(), f.error) && asset == intactAsset, "invalid interface version fails transactionally");
    MaterialGraphDocument loaded;
    Check(f.graph.ToJson().find("\"version\": 4") != std::string::npos && loaded.FromJson(f.graph.ToJson(), f.error) && loaded == f.graph, "graph v4 round-trip preserves call GUID, version, ports and defaults");
    auto malformed = f.graph; malformed.FindNode(f.call)->ports[1].id = malformed.FindNode(f.call)->ports[0].id;
    Check(!loaded.FromJson(malformed.ToJson(), f.error) && loaded == f.graph, "duplicate interface port IDs are rejected without replacing the graph");
    auto source = f.graph; MaterialGraphProgram program;
    Check(program.Compile(f.graph, f.error, PinId(f.call, 1), f.Resolver()), "subgraph expands into the existing material compiler");
    std::vector<std::array<GraphValue, 14>> work;
    Check(Close(program.Evaluate({}, {}, {}, work).roughness, .5f) && Close(program.PreviewValue(work).number[0], .5f), "CPU evaluation and selected call-output preview use declared defaults");
    Check(source == f.graph && std::none_of(program.Instructions().begin(), program.Instructions().end(), [](const auto& i) { return IsSubgraphNode(i.node.kind); }), "saved definition is unchanged and IR contains no structural placeholder operations");
    Check(program.Layout().dependencyGuids == std::vector<std::string>{A} && !program.Layout().subgraphOrigins.empty(), "compiler reports subgraph dependencies and generated-node provenance");
    auto parameters = ParameterOverrides{}; parameters.values["Subgraph_" + std::to_string(f.call) + "/Scale"] = {1.5f, 0, 0, 0};
    Check(program.ValidateOverrides(parameters, f.error) && Close(program.Evaluate({}, parameters, {}, work).roughness, .75f), "published subgraph parameters have a stable call scope and support instance overrides");
    const auto hlsl = program.GenerateHlsl();
    Check(hlsl.find(A)!=std::string::npos,"generated HLSL identifies the source asset and node for expanded instructions");
    std::reverse(f.graph.nodes.begin(), f.graph.nodes.end()); std::reverse(f.graph.links.begin(), f.graph.links.end());
    Check(program.Compile(f.graph, f.error, PinId(f.call, 1), f.Resolver()) && program.GenerateHlsl() == hlsl, "expansion and HLSL are deterministic under stored array reordering");
    MaterialGraphProgram standalone;
    Check(standalone.Compile(asset.graph, f.error, PinId(4, 1)), "standalone subgraph authoring previews its output using numeric input defaults");
    standalone.Evaluate({}, {}, {}, work); Check(Close(standalone.PreviewValue(work).number[0], .5f), "authoring output declaration is resolved through the same evaluator");
    BakedMaterialImages images; Check(BakeMaterialUV(program, parameters, {}, 16, images, f.error) && images.roughness[0] == 191, "expanded UV subgraph bakes through the existing reference baker");
    const auto second=f.graph.AddNode(NodeKind::Subgraph,{});
    BindMaterialSubgraph(f.graph,second,A,f.assets[A],f.error);
    const auto sum=f.graph.AddNode(NodeKind::Add,{});
    f.graph.Connect(PinId(f.call,1),PinId(sum,0),f.error);f.graph.Connect(PinId(second,1),PinId(sum,1),f.error);f.graph.Connect(PinId(sum,2),PinId(3,1),f.error);
    parameters.values["Subgraph_"+std::to_string(second)+"/Scale"]={.25f,0,0,0};
    Check(program.Compile(f.graph,f.error,0,f.Resolver()) && Close(program.Evaluate({},parameters,{},work).roughness,.875f),"repeated calls share the asset but keep separate published-parameter scopes");
}
void NestedCallsAndFailures()
{
    Fixture f; auto nested = f.assets.at(A);
    const auto call = nested.graph.AddNode(NodeKind::Subgraph, {});
    Check(BindMaterialSubgraph(nested.graph, call, B, f.assets.at(A), f.error), "nested call binds to another GUID-backed subgraph");
    nested.graph.Connect(PinId(1, 0), PinId(call, 0), f.error);
    nested.graph.Connect(PinId(call, 1), PinId(3, 0), f.error);
    f.assets[B] = f.assets[A]; f.assets[A] = nested;
    MaterialGraphProgram program; std::vector<std::array<GraphValue, 14>> work;
    Check(program.Compile(f.graph, f.error, 0, f.Resolver()) && program.Layout().dependencyGuids == std::vector<std::string>{A, B}, "recursive expansion records the complete sorted dependency closure");
    ParameterOverrides overrides; overrides.values["Subgraph_" + std::to_string(f.call) + "/Subgraph_" + std::to_string(call) + "/Scale"] = {.5f, 0, 0, 0};
    Check(program.ValidateOverrides(overrides, f.error) && Close(program.Evaluate({}, overrides, {}, work).roughness, .25f), "nested published parameters remain independently addressable");
    const auto shader = program.GenerateHlsl();
    Check(!program.Compile(f.graph, f.error) && !f.error.empty() && program.GenerateHlsl() == shader, "missing resolver preserves the last valid material program");
    f.assets.erase(B);
    Check(!program.Compile(f.graph, f.error, 0, f.Resolver()) && f.error.find(B) != std::string::npos && program.GenerateHlsl() == shader, "missing nested asset reports its GUID and preserves valid shader data");
    f.assets[B] = nested;
    // A -> B -> B: the body is valid; the asset dependency graph is recursive.
    Check(!program.Compile(f.graph, f.error, 0, f.Resolver()) && f.error.find("Recursive subgraph") != std::string::npos && program.GenerateHlsl() == shader, "recursive asset dependencies fail without stack overflow or partial program replacement");
    f.assets[B] = MaterialSubgraph::Example(); ++f.assets[A].interfaceVersion;
    Check(!program.Compile(f.graph, f.error, 0, f.Resolver()) && f.error.find("interface") != std::string::npos, "changed interface versions require an explicit refresh");
    Check(BindMaterialSubgraph(f.graph, f.call, A, f.assets[A], f.error) && program.Compile(f.graph, f.error, 0, f.Resolver()), "explicit compatible refresh preserves existing links");
    const auto before = f.graph;
    f.assets[A].graph.FindNode(4)->interfaceType = PinType::Vector3;
    // Create a valid changed output using a Vector3 source.
    const auto vector = f.assets[A].graph.AddNode(NodeKind::Vector3, {});
    f.assets[A].graph.Connect(PinId(vector, 0), PinId(4, 0), f.error);
    Check(!BindMaterialSubgraph(f.graph, f.call, A, f.assets[A], f.error) && f.graph == before, "refresh refuses to lose a connected type-changed output");
    MaterialGraphDocument expanded = before; MaterialSubgraphExpansion info; info.previewPin = 123;
    Check(!ExpandMaterialSubgraphs(before, {}, expanded, info, f.error) && expanded == before && info.previewPin == 123, "failed expansion preserves both public result objects");
}
void TypesLayersAndLimits()
{
    const auto identity = [](PinType type) {
        MaterialSubgraph asset; std::string error;
        const auto input = asset.graph.AddNode(NodeKind::SubgraphInput, {}), output = asset.graph.AddNode(NodeKind::SubgraphOutput, {});
        asset.graph.FindNode(input)->interfaceType = type; asset.graph.FindNode(output)->interfaceType = type;
        asset.graph.Connect(PinId(input, 0), PinId(output, 0), error); return asset;
    };
    for (auto type : {PinType::Color, PinType::Vector2, PinType::Vector3, PinType::Texture, PinType::Surface}) {
        auto asset = identity(type); MaterialGraphDocument graph; std::string error;
        const auto call = graph.AddNode(NodeKind::Subgraph, {}); BindMaterialSubgraph(graph, call, A, asset, error);
        const auto kind = type == PinType::Color ? NodeKind::Color : type == PinType::Vector2 ? NodeKind::Vector2 : type == PinType::Vector3 ? NodeKind::Vector3 : type == PinType::Texture ? NodeKind::TextureParameter : NodeKind::MaterialLayer;
        const auto source = graph.AddNode(kind, {}); const auto sourceSlot = type == PinType::Surface ? 8 : 0;
        graph.FindNode(source)->value={.2f,.3f,.4f,.8f};
        graph.Connect(PinId(source, sourceSlot), PinId(call, 0), error);
        const auto output = graph.AddNode(type == PinType::Texture ? NodeKind::TextureSample : NodeKind::PbrOutput, {});
        const auto targetSlot = type == PinType::Surface ? 8 : type == PinType::Vector3 ? 4 : 0;
        if (type == PinType::Vector2) graph.Connect(PinId(call, 1), PinId(graph.AddNode(NodeKind::TextureSample, {}), 1), error);
        else graph.Connect(PinId(call, 1), PinId(output, targetSlot), error);
        MaterialGraphProgram program;
        const auto resolver = [&](const auto&, auto& result, auto&) { result = asset; return true; };
        Check(program.Compile(graph, error, type == PinType::Texture ? PinId(output, 2) : PinId(call, 1), resolver), "all six pin types survive subgraph passthrough expansion");
        std::vector<std::array<GraphValue,14>> work;program.Evaluate({}, {}, {},work);
        const auto actual=program.PreviewValue(work);
        Check(Close(type==PinType::Surface?actual.surface.color[0]:actual.number[0],type==PinType::Texture?SrgbToLinear(.2f):.2f),"typed passthrough preserves reference values and texture RGB decoding");
        if (type == PinType::Texture || type == PinType::Surface) {
            auto missing = graph; std::erase_if(missing.links, [&](const auto& l) { return l.input == PinId(call, 0); });
            Check(!program.Compile(missing, error, 0, resolver) && error.find("unbound") != std::string::npos, "unbound resource/surface inputs produce a diagnostic instead of invented data");
        }
    }
    Fixture f; MaterialGraphDocument layers; const auto base = layers.AddMaterialLayer("Base", f.error), top = layers.AddMaterialLayer("Top", f.error);
    (void)base; const auto call = layers.AddNode(NodeKind::Subgraph, {}); BindMaterialSubgraph(layers, call, A, f.assets[A], f.error);
    Check(layers.SetMaterialLayerMask(top, PinId(call, 1), f.error), "a reusable subgraph output can be a managed layer mask");
    MaterialGraphProgram program; Check(program.Compile(layers, f.error, 0, f.Resolver()), "expansion retains canonical managed-layer topology");
    auto limited = f.graph; limited.nextId = 0x01000000;
    Check(!program.Compile(limited, f.error, 0, f.Resolver()) && f.error.find("IDs") != std::string::npos, "expansion detects exhausted stable-ID space");
    auto deep = MaterialSubgraph::Example();
    for (unsigned i = 0; i < 17; ++i) {
        auto parent = MaterialSubgraph::Example(); const auto child = parent.graph.AddNode(NodeKind::Subgraph, {});
        const auto guid = "f3333333-3333-4333-8333-" + std::string(11 - std::to_string(i).size(), '0') + "1" + std::to_string(i);
        BindMaterialSubgraph(parent.graph, child, guid, deep, f.error); parent.graph.Connect(PinId(child, 1), PinId(3, 0), f.error);
        f.assets[guid] = deep; deep = parent;
    }
    f.assets[A] = deep;
    Check(!program.Compile(f.graph, f.error, 0, f.Resolver()) && f.error.find("depth") != std::string::npos, "deep asset nesting is bounded before recursive expansion");
    Fixture geometry; const auto curvature=geometry.assets[A].graph.AddNode(NodeKind::Curvature,{});
    geometry.assets[A].graph.Connect(PinId(curvature,0),PinId(4,0),geometry.error);
    Check(program.Compile(geometry.graph,geometry.error,0,geometry.Resolver()) && program.NeedsSurfaceData() && !program.CanBakeUV(),"geometry requirements propagate out of subgraphs into the shared compiler");
    EvaluationContext context;context.curvature=.73f;std::vector<std::array<GraphValue,14>> work;
    Check(Close(program.Evaluate(context,{}, {},work).roughness,.73f),"geometry subgraph reads the explicit supplied reference context");
    Fixture size;
    while(size.assets[A].graph.nodes.size()<2048)size.assets[A].graph.AddNode(NodeKind::Scalar,{});
    Check(!program.Compile(size.graph,size.error,0,size.Resolver()) && size.error.find("2048")!=std::string::npos,"expanded node count is bounded even when the source asset itself fits its limit");
    Fixture calls;
    for (unsigned i=0;i<256;++i) { const auto id=calls.graph.AddNode(NodeKind::Subgraph,{});BindMaterialSubgraph(calls.graph,id,A,calls.assets[A],calls.error); }
    Check(!program.Compile(calls.graph,calls.error,0,calls.Resolver()) && calls.error.find("256 calls")!=std::string::npos,"subgraph call fanout is bounded independently of dependency cycles");
}
void Shader()
{
    Fixture f; MaterialGraphProgram program; Check(program.Compile(f.graph, f.error, 0, f.Resolver()), "shader fixture expands");
    const auto source = std::filesystem::path(IX_SUBGRAPH_SHADER_OUTPUT); std::filesystem::create_directories(source.parent_path());
    std::ofstream(source) << program.GenerateHlsl();
#if defined(_WIN32) && defined(IX_GRAPH_DXC)
    const auto dxc = std::filesystem::path(IX_GRAPH_DXC).wstring(), shader = source.wstring(), dxil = shader + L".dxil", spv = shader + L".spv";
    const wchar_t* args[] = {dxc.c_str(), L"-T", L"ps_6_0", L"-E", L"GraphValidationPS", L"-Fo", dxil.c_str(), shader.c_str(), nullptr};
    Check(_wspawnv(_P_WAIT, dxc.c_str(), args) == 0, "expanded subgraph HLSL compiles to DXIL");
    const wchar_t* spirv[] = {dxc.c_str(), L"-spirv", L"-T", L"ps_6_0", L"-E", L"GraphValidationPS", L"-Fo", spv.c_str(), shader.c_str(), nullptr};
    Check(_wspawnv(_P_WAIT, dxc.c_str(), spirv) == 0, "expanded subgraph HLSL compiles to SPIR-V");
#else
    std::cout << "SKIP DXC not configured; no GPU compilation claim\n";
#endif
}
void Benchmark()
{
    Fixture f;std::uint32_t previous=PinId(f.call,1);
    for (unsigned index=1;index<32;++index) {
        const auto call=f.graph.AddNode(NodeKind::Subgraph,{});BindMaterialSubgraph(f.graph,call,A,f.assets[A],f.error);
        const auto add=f.graph.AddNode(NodeKind::Add,{});f.graph.Connect(previous,PinId(add,0),f.error);f.graph.Connect(PinId(call,1),PinId(add,1),f.error);previous=PinId(add,2);
    }
    f.graph.Connect(previous,PinId(3,1),f.error);
    const auto median=[](auto operation) {
        operation();std::vector<double> times;
        for(unsigned run=0;run<5;++run) {const auto start=std::chrono::steady_clock::now();operation();times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
        std::sort(times.begin(),times.end());return times[2];
    };
    unsigned resolverCalls=0;
    const auto resolver=[&](const auto& guid,auto& asset,auto&) {++resolverCalls;asset=f.assets.at(guid);return true;};
    auto program=std::make_shared<MaterialGraphProgram>();
    const auto compile=median([&]{resolverCalls=0;if(!program->Compile(f.graph,f.error,0,resolver))++failures;});
    std::size_t hlslBytes=0;const auto hlsl=median([&]{hlslBytes=program->GenerateHlsl().size();});
    std::size_t previewBytes=0;const auto preview=median([&]{previewBytes=RenderMaterialPreview(*program,{}, {},{},128).size();});
    MaterialInstance instance;instance.definitionGuid=B;MaterialInstanceBinding binding;
    if(!binding.Bind(B,program,instance,f.error))++failures;
    ParameterOverrides parameters;parameters.values["Subgraph_"+std::to_string(f.call)+"/Scale"]={.37f,0,0,0};
    const auto updates=median([&]{for(unsigned run=0;run<1000;++run)if(!binding.Update(parameters,f.error))++failures;});
    std::cout<<"PROFILE 32 connected calls, one in-memory GUID asset, median_of_5_after_warmup\n"
        <<"subgraph_compile_32_calls_ms="<<compile<<"\nsubgraph_hlsl_32_calls_ms="<<hlsl<<"\nsubgraph_preview_128_ms="<<preview
        <<"\nsubgraph_instance_1000_updates_ms="<<updates<<"\nresolver_calls_per_compile="<<resolverCalls
        <<"\nreachable_instructions="<<program->Instructions().size()<<"\nparameter_buffer_bytes="<<binding.ParameterBuffer().size()*sizeof(Float4)
        <<"\nhlsl_bytes="<<hlslBytes<<"\npreview_bytes="<<previewBytes<<'\n';
    std::cout<<"CPU/in-memory resolver reference only; no file IO, GPU execution or real geometry bake in this benchmark.\n";
}
}
int main(int argc,char** argv) { if(argc>1 && std::string(argv[1])=="--benchmark")Benchmark();else {RoundTripsAndEvaluation();NestedCallsAndFailures();TypesLayersAndLimits();Shader();} std::cout << "Subgraph failures: " << failures << '\n'; return failures ? 1 : 0; }
