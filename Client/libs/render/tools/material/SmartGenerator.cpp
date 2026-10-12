#include "SmartGenerator.h"
#include <crude_json.h>
#include <algorithm>

namespace material_tool {
namespace {
using Json=crude_json::value;
constexpr SmartGeneratorPreset Presets[]={
    {SmartGeneratorKind::EdgeWear,"edge-wear","Edge Wear","Convex curvature wear with transition width and seeded breakup."},
    {SmartGeneratorKind::DirtAccumulation,"dirt-accumulation","Dirt Accumulation","Cavity and occluded recesses with seeded dirt breakup."},
    {SmartGeneratorKind::RustSpread,"rust-spread","Rust Spread","Independent edge/cavity influences and seeded rust coverage."},
    {SmartGeneratorKind::MossGrowth,"moss-growth","Moss Growth","Growth-facing slopes, projected height, cavity and seeded growth."},
    {SmartGeneratorKind::DustAccumulation,"dust-accumulation","Dust Accumulation","Up-facing slopes, sheltered AO and seeded dust breakup."},
    {SmartGeneratorKind::WaterStreaks,"water-streaks","Water Streaks","Gravity-aligned anisotropic noise with streak width and length."}
};
const SmartGeneratorPreset* Find(SmartGeneratorKind kind) {for(const auto& p:Presets)if(p.kind==kind)return &p;return nullptr;}
struct Pin {std::uint32_t node;size_t slot=0;};
class Recipe {
public:
    MaterialSubgraph asset;
    std::string error;
    Pin Node(NodeKind kind) {
        const auto index=asset.graph.nodes.size();return {asset.graph.AddNode(kind,{static_cast<float>((index/10)*280),static_cast<float>((index%10)*130)})};
    }
    void Connect(Pin from,Pin to) {if(error.empty())asset.graph.Connect(PinId(from.node,from.slot),PinId(to.node,to.slot),error);}
    Pin Constant(float value) {auto p=Node(NodeKind::Scalar);asset.graph.FindNode(p.node)->value[0]=value;return p;}
    Pin Parameter(const char* name,float value) {auto p=Node(NodeKind::ScalarParameter);auto& n=*asset.graph.FindNode(p.node);n.parameter=n.name=name;n.value[0]=value;return p;}
    Pin Binary(NodeKind kind,Pin a,Pin b) {auto p=Node(kind);Connect(a,{p.node,0});Connect(b,{p.node,1});return {p.node,2};}
    Pin Unary(NodeKind kind,Pin a) {auto p=Node(kind);Connect(a,p);return {p.node,1};}
    Pin Range(Pin value,float low,float high) {auto p=Node(NodeKind::Clamp);auto& n=*asset.graph.FindNode(p.node);n.value[1]=low;n.value[2]=high;Connect(value,p);return {p.node,3};}
    Pin Mix(Pin a,Pin b,Pin weight) {auto p=Node(NodeKind::Lerp);Connect(a,p);Connect(b,{p.node,1});Connect(weight,{p.node,2});return {p.node,3};}
    Pin Smooth(Pin a,Pin b,Pin value) {auto p=Node(NodeKind::Smoothstep);Connect(value,p);Connect(a,{p.node,1});Connect(b,{p.node,2});return {p.node,3};}
    Pin Sat(Pin a) {return Unary(NodeKind::Saturate,a);}
    Pin Add(Pin a,Pin b) {return Binary(NodeKind::Add,a,b);}
    Pin Mul(Pin a,Pin b) {return Binary(NodeKind::Multiply,a,b);}
    Pin Max(Pin a,float b) {return Binary(NodeKind::Maximum,a,Constant(b));}
    Pin Dot(Pin a,Pin b) {return Binary(NodeKind::Dot,a,b);}
    std::array<Pin,3> Split(Pin value) {auto p=Node(NodeKind::SplitVector);Connect(value,p);return {{{p.node,1},{p.node,2},{p.node,3}}};}
    Pin Vector(std::array<Pin,3> values) {auto p=Node(NodeKind::MakeVector);for(int k=0;k<3;++k)Connect(values[k],{p.node,static_cast<size_t>(k)});return {p.node,3};}
    Pin Direction(const char* name,std::array<float,4> fallback) {
        auto p=Node(NodeKind::Vector3Parameter);auto& n=*asset.graph.FindNode(p.node);n.name=n.parameter=name;n.value=fallback;
        auto components=Split(p);for(auto& c:components)c=Range(c,-1000000,1000000);
        const auto bounded=Vector(components),length=Unary(NodeKind::Length,bounded);
        const auto valid=Binary(NodeKind::Step,Constant(.0001f),length);
        for(int k=0;k<3;++k)components[k]=Mix(Constant(fallback[k]),components[k],valid);
        return Unary(NodeKind::Normalize,Vector(components));
    }
    Pin Noise(Pin position,bool water=false) {
        auto noise=Node(NodeKind::Noise);auto& n=*asset.graph.FindNode(noise.node);n.value={3,4,.5f,19};n.settings[0]=1;
        const auto scale=Node(NodeKind::VectorScale);Connect(position,scale);
        Connect(Range(Parameter("Scale",water?1.0f:3.0f),.001f,1000),{scale.node,1});
        // The shared IR finite-output bound applies before fBm's octave scaling.
        Connect({scale.node,2},noise);Connect(Constant(1),{noise.node,1});
        Connect(Unary(NodeKind::Floor,Range(Parameter("Seed",19),0,1000000)),{noise.node,3});
        auto input=Node(NodeKind::SubgraphInput);auto& port=*asset.graph.FindNode(input.node);port.parameter=port.name="NoiseMask";port.value[0]=1;
        return Mix(Constant(1),Mul({noise.node,2},Sat(input)),Sat(Parameter("NoiseInfluence",.5f)));
    }
    Pin Finish(Pin signal,const char* amount) {
        const auto contrast=Max(Parameter("Contrast",1),.001f);
        return Sat(Mul(Binary(NodeKind::Power,Sat(signal),contrast),Sat(Parameter(amount,1))));
    }
    void Output(Pin mask) {auto output=Node(NodeKind::SubgraphOutput);auto& n=*asset.graph.FindNode(output.node);n.name=n.parameter="Mask";Connect(mask,output);}
};
}
std::span<const SmartGeneratorPreset> SmartGeneratorPresets() {return Presets;}
bool SmartGenerator::Create(SmartGeneratorKind kind,SmartGenerator& result,std::string& error) {
    error.clear();const auto* preset=Find(kind);if(!preset){error="Unknown built-in smart generator";return false;}
    Recipe r;const auto position=r.Node(NodeKind::WorldPosition);Pin signal{};const char* amount="Amount";
    if(kind==SmartGeneratorKind::WaterStreaks) {
        const auto gravity=r.Direction("GravityDirection",{0,-1,0,0});const auto along=r.Dot(position,gravity);
        const auto width=r.Range(r.Parameter("StreakWidth",.15f),.001f,1000000),length=r.Range(r.Parameter("StreakLength",4),.001f,1000000);
        auto p=r.Split(position),g=r.Split(gravity);std::array<Pin,3> stretched;
        const auto longitudinal=r.Binary(NodeKind::Divide,along,length);
        for(int k=0;k<3;++k)stretched[k]=r.Add(r.Binary(NodeKind::Divide,r.Binary(NodeKind::Subtract,p[k],r.Mul(g[k],along)),width),r.Mul(g[k],longitudinal));
        const auto noise=r.Noise(r.Vector(stretched),true),threshold=r.Sat(r.Parameter("StreakThreshold",.55f)),softness=r.Range(r.Parameter("StreakSoftness",.12f),.001f,1);
        signal=r.Smooth(r.Binary(NodeKind::Subtract,threshold,softness),r.Add(threshold,softness),noise);amount="StreakAmount";
    } else {
        const auto noise=r.Noise(position);
        switch(kind) {
        case SmartGeneratorKind::EdgeWear: {
            const auto width=r.Range(r.Parameter("EdgeWidth",.25f),.001f,.5f);
            signal=r.Smooth(r.Constant(.5f),r.Add(r.Constant(.5f),width),r.Node(NodeKind::Curvature));amount="WearAmount";break;
        }
        case SmartGeneratorKind::DirtAccumulation:
            signal=r.Sat(r.Binary(NodeKind::Maximum,r.Node(NodeKind::Cavity),r.Unary(NodeKind::MaskInvert,r.Node(NodeKind::AO))));amount="DirtAmount";break;
        case SmartGeneratorKind::RustSpread: {
            const auto edge=r.Sat(r.Mul(r.Binary(NodeKind::Subtract,r.Node(NodeKind::Curvature),r.Constant(.5f)),r.Constant(2)));
            signal=r.Sat(r.Add(r.Mul(edge,r.Sat(r.Parameter("EdgeInfluence",.5f))),r.Mul(r.Node(NodeKind::Cavity),r.Sat(r.Parameter("CavityInfluence",.75f)))));amount="RustAmount";break;
        }
        case SmartGeneratorKind::MossGrowth: {
            const auto direction=r.Direction("GrowthDirection",{0,1,0,0});
            const auto slope=r.Smooth(r.Constant(.2f),r.Constant(.8f),r.Sat(r.Dot(r.Node(NodeKind::WorldNormal),direction)));
            const auto height=r.Unary(NodeKind::MaskInvert,r.Smooth(r.Parameter("HeightMin",0),r.Parameter("HeightMax",10),r.Dot(position,direction)));
            signal=r.Mul(r.Sat(r.Add(slope,r.Mul(r.Node(NodeKind::Cavity),r.Sat(r.Parameter("CavityInfluence",.25f))))),r.Mix(r.Constant(1),height,r.Sat(r.Parameter("HeightInfluence",0))));amount="MossAmount";break;
        }
        case SmartGeneratorKind::DustAccumulation: {
            const auto slope=r.Node(NodeKind::SlopeMask);r.asset.graph.FindNode(slope.node)->value={20,70,0,0};
            signal=r.Mul(r.Unary(NodeKind::MaskInvert,{slope.node,3}),r.Mix(r.Constant(1),r.Unary(NodeKind::MaskInvert,r.Node(NodeKind::AO)),r.Sat(r.Parameter("ShelterInfluence",.25f))));amount="DustAmount";break;
        }
        default:break;
        }
        signal=r.Mul(signal,noise);
    }
    r.Output(r.Finish(signal,amount));if(!r.error.empty()){error=r.error;return false;}
    SmartGenerator draft;draft.kind=kind;draft.name=preset->name;draft.description=preset->description;draft.definition=std::move(r.asset);
    if(!draft.Validate(error))return false;result=std::move(draft);return true;
}
bool SmartGenerator::Validate(std::string& error) const {
    error.clear();if((!Find(kind) && kind!=SmartGeneratorKind::Custom) || name.empty() || name.size()>255 || description.size()>4096){error="Invalid smart generator metadata";return false;}
    std::vector<MaterialGraphPort> ports;if(!definition.GetInterface(ports,error))return false;
    unsigned outputs=0;for(const auto& p:ports)if(!p.input){++outputs;if(p.type!=PinType::Scalar || p.name!="Mask"){error="Smart generator must output one Scalar port named Mask";return false;}}
    if(outputs!=1){error="Smart generator must have exactly one mask output";return false;}return true;
}
std::string SmartGenerator::ToJson() const {
    Json root(crude_json::type_t::object);root["format"]="ixtreeme.smartgenerator";root["version"]=1.0;
    root["kind"]=Find(kind)?Find(kind)->id:"custom";root["name"]=name;root["description"]=description;root["subgraph"]=Json::parse(definition.ToJson());return root.dump(2);
}
bool SmartGenerator::FromJson(const std::string& text,std::string& error) {
    error.clear();if(text.size()>4*1024*1024){error="Smart generator exceeds 4 MB";return false;}
    const auto root=Json::parse(text);
    if(!root.is_object() || !root.contains("format") || !root["format"].is_string() || root["format"].get<crude_json::string>()!="ixtreeme.smartgenerator" || !root.contains("version") || !root["version"].is_number() || root["version"].get<crude_json::number>()!=1 || !root.contains("kind") || !root["kind"].is_string() || !root.contains("name") || !root["name"].is_string() || !root.contains("description") || !root["description"].is_string() || !root.contains("subgraph")){error="Invalid or unsupported smart generator format";return false;}
    SmartGenerator draft;const auto key=root["kind"].get<crude_json::string>();bool found=key=="custom";
    for(const auto& p:Presets)if(key==p.id){draft.kind=p.kind;found=true;break;}
    if(!found){error="Unknown smart generator kind";return false;}
    draft.name=root["name"].get<crude_json::string>();draft.description=root["description"].get<crude_json::string>();
    if(!draft.definition.FromJson(root["subgraph"].dump(),error) || !draft.Validate(error))return false;*this=std::move(draft);return true;
}
std::uint32_t AddSmartGenerator(MaterialGraphDocument& graph,const std::string& guid,const SmartGenerator& generator,std::uint32_t layer,std::string& error,std::array<float,2> position) {
    if(!generator.Validate(error))return 0;auto draft=graph;const auto call=draft.AddNode(NodeKind::Subgraph,position);
    if(!call){error="Cannot allocate smart generator node";return 0;}
    if(!BindMaterialSubgraph(draft,call,guid,generator.definition,error))return 0;
    auto& node=*draft.FindNode(call);node.name=generator.name;
    if(layer){const auto mask=std::find_if(node.ports.begin(),node.ports.end(),[](const auto& p){return !p.input;});const auto slot=static_cast<size_t>(mask-node.ports.begin());if(!draft.SetMaterialLayerMask(layer,PinId(call,slot),error))return 0;}
    graph=std::move(draft);return call;
}
}
