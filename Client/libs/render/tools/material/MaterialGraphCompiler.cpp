#include "MaterialGraphCompiler.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace material_tool
{
namespace
{
float Sat(float v) { return std::clamp(v, 0.0f, 1.0f); }
float SafeDivide(float a, float b) { return std::abs(b) < 1e-6f ? 0.0f : a / b; }
float Smooth(float a, float b, float x) { const float t = std::abs(b-a) < 1e-6f ? (x >= b ? 1.0f : 0.0f) : Sat((x-a)/(b-a)); return t*t*(3-2*t); }
Float4 Mix(Float4 a, Float4 b, float t) { for (int i=0;i<4;++i) a[i] += (b[i]-a[i])*t; return a; }
Float4 Math(Float4 a, Float4 b, bool multiply) { for(int i=0;i<4;++i) a[i] = multiply ? a[i]*b[i] : a[i]+b[i]; return a; }
float Dot3(Float4 a, Float4 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Float4 NormalDetail(Float4 a, Float4 b, float strength)
{
    a = Normalize3(a); b = Normalize3(Mix({0,0,1,0}, Normalize3(b), Sat(strength)));
    const Float4 t{a[0],a[1],a[2]+1,0}, u{-b[0],-b[1],b[2],0};
    if (std::abs(t[2]) < 1e-6f) return a;
    const float d = Dot3(t,u)/t[2]; return Normalize3({t[0]*d-u[0], t[1]*d-u[1], t[2]*d-u[2],0});
}
Surface MixSurface(Surface a, const Surface& b, float t, unsigned channels = 255)
{
    t = Sat(t);
    if (channels & 1) a.color=Mix(a.color,b.color,t);
    if (channels & 2) a.roughness += (b.roughness-a.roughness)*t;
    if (channels & 4) a.metallic += (b.metallic-a.metallic)*t;
    if (channels & 8) a.emissive=Mix(a.emissive,b.emissive,t);
    if (channels & 16) a.normal=Normalize3(Mix(a.normal,b.normal,t));
    if (channels & 32) a.ao += (b.ao-a.ao)*t;
    if (channels & 64) a.height += (b.height-a.height)*t;
    if (channels & 128) a.opacity += (b.opacity-a.opacity)*t;
    return a;
}
std::uint32_t Hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; return x ^ (x >> 16); }
float Lattice(int x, int y, int z, std::uint32_t seed)
{
    return static_cast<float>(Hash(static_cast<std::uint32_t>(x)*73856093U ^ static_cast<std::uint32_t>(y)*19349663U ^ static_cast<std::uint32_t>(z)*83492791U ^ seed)&0x00ffffffU)/16777215.0f;
}
float Noise(Float4 p, int octaves, float persistence, std::uint32_t seed)
{
    float sum=0, weight=0, amplitude=1;
    for(int o=0;o<octaves;++o) {
        // Bound lattice coordinates before conversion; repeat period is 4096 units per octave.
        Float4 q{}; for(int i=0;i<3;++i) q[i]=std::fmod(p[i],4096.0f);
        const int x=static_cast<int>(std::floor(q[0])), y=static_cast<int>(std::floor(q[1])), z=static_cast<int>(std::floor(q[2]));
        const float u=Smooth(0,1,q[0]-x), v=Smooth(0,1,q[1]-y), w=Smooth(0,1,q[2]-z);
        const auto lerp=[](float a,float b,float t){return a+(b-a)*t;};
        const float n=lerp(lerp(lerp(Lattice(x,y,z,seed),Lattice(x+1,y,z,seed),u),lerp(Lattice(x,y+1,z,seed),Lattice(x+1,y+1,z,seed),u),v),
            lerp(lerp(Lattice(x,y,z+1,seed),Lattice(x+1,y,z+1,seed),u),lerp(Lattice(x,y+1,z+1,seed),Lattice(x+1,y+1,z+1,seed),u),v),w);
        sum+=n*amplitude; weight+=amplitude; amplitude*=persistence; for(int i=0;i<3;++i)p[i]*=2;
    } return sum/std::max(weight,1e-6f);
}
std::string Float(float f) { std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(9) << f; auto r=s.str(); if(r.find_first_of(".eE")==std::string::npos)r+=".0"; return r; }
std::string Literal(Float4 v, int n=4) { std::string r="float"+std::to_string(n)+"("; for(int i=0;i<n;++i) {if(i)r+=",";r+=Float(v[i]);} return r+")"; }
}
Float4 Normalize3(Float4 v, Float4 fallback) { const float length=std::sqrt(Dot3(v,v)); if(length<1e-6f || !std::isfinite(length))return fallback; for(int i=0;i<3;++i)v[i]/=length; v[3]=0; return v; }
float SrgbToLinear(float v) { return v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f); }
float LinearToSrgb(float v) { v=std::max(v,0.0f); return v<=0.0031308f ? 12.92f*v : 1.055f*std::pow(v,1/2.4f)-0.055f; }
bool SameEvaluationGraph(const MaterialGraphDocument& a, const MaterialGraphDocument& b)
{
    if(a.nodes.size()!=b.nodes.size() || a.links!=b.links)return false;
    for(size_t i=0;i<a.nodes.size();++i) {
        const auto& x=a.nodes[i]; const auto& y=b.nodes[i];
        if(x.id!=y.id || x.kind!=y.kind || x.value!=y.value || x.settings!=y.settings || x.resource!=y.resource || x.assetGuid!=y.assetGuid || x.assetId!=y.assetId || x.parameter!=y.parameter || x.srgb!=y.srgb || x.interfaceType!=y.interfaceType || x.interfaceVersion!=y.interfaceVersion || x.ports!=y.ports)return false;
    } return true;
}
bool MaterialGraphProgram::Compile(const MaterialGraphDocument& document, std::string& error, std::uint32_t previewPin,
                                   const MaterialSubgraphResolver& subgraphs)
{
    error.clear(); MaterialGraphProgram draft;
    // Revalidate programmatically assembled documents as well as documents loaded from disk.
    MaterialGraphDocument valid; if(!valid.FromJson(document.ToJson(),error)) return false;
    if (std::any_of(valid.nodes.begin(), valid.nodes.end(), [](const auto& n) { return IsSubgraphNode(n.kind); })) {
        MaterialSubgraphExpansion expansion;
        if (!ExpandMaterialSubgraphs(document, subgraphs, valid, expansion, error, previewPin)) return false;
        previewPin = expansion.previewPin;
        draft.layout_.dependencyGuids = std::move(expansion.dependencyGuids);
        draft.layout_.subgraphOrigins = std::move(expansion.origins);
    }
    // Stored array order is editor data, not program identity or binding order.
    std::sort(valid.nodes.begin(), valid.nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(valid.links.begin(), valid.links.end(), [](const auto& a, const auto& b) { return a.input < b.input; });
    std::unordered_map<std::uint32_t,int> indices, degrees;
    std::unordered_map<std::uint32_t,const GraphNode*> nodes;
    std::unordered_map<std::uint32_t,GraphLink> inputs;
    std::unordered_map<std::uint32_t,std::vector<std::uint32_t>> dependencies, outgoing;
    for(const auto& n:valid.nodes)nodes[n.id]=&n;
    for (const auto& n : valid.nodes) if ((n.kind == NodeKind::LayerBlend || n.kind == NodeKind::HeightBlend) && n.settings[3] == 1 &&
        (n.settings[2] < 0 || n.settings[2] > 255 || std::floor(n.settings[2]) != n.settings[2])) { error = "Invalid layer channel mask"; return false; }
    for (const auto& n : valid.nodes) if (n.kind == NodeKind::MaskCombine && (n.settings[0] < 0 || n.settings[0] > 4 || std::floor(n.settings[0]) != n.settings[0]))
    { error = "Unsupported mask combine operation at node " + std::to_string(n.id); return false; }
    for(const auto& l:valid.links){inputs[l.input]=l;dependencies[l.input/16].push_back(l.output/16);outgoing[l.output/16].push_back(l.input/16);}
    std::unordered_set<std::string> parameters;
    for(const auto& n:valid.nodes) if(n.kind==NodeKind::ScalarParameter || n.kind==NodeKind::ColorParameter || n.kind==NodeKind::TextureParameter || n.kind==NodeKind::Vector2Parameter || n.kind==NodeKind::Vector3Parameter) {
        if(n.parameter.empty() || !parameters.insert(n.parameter).second) { error="Node "+std::to_string(n.id)+": parameter names must be nonempty and unique";return false; }
        const auto type = NodePins(n.kind)[0].type;
        draft.layout_.parameters.push_back({n.id, n.parameter, type, n.value, draft.layout_.parameterBufferBytes});
        if (type != PinType::Texture) draft.layout_.parameterBufferBytes += sizeof(Float4);
    }
    const auto output=std::find_if(valid.nodes.begin(),valid.nodes.end(),[](const auto& n){return n.kind==NodeKind::PbrOutput;});
    std::vector<std::uint32_t> pending;
    if(output!=valid.nodes.end())pending.push_back(output->id);
    if(previewPin) {
        const auto pin=valid.FindPin(previewPin);
        if(!pin || pin->definition.input || pin->definition.type==PinType::Texture){error="Select a numeric or surface output to preview";return false;}
        pending.push_back(pin->node);draft.previewSlot_=static_cast<int>(previewPin%16)-1;
    }
    if(pending.empty()){error="Add a PBR Output node";return false;}
    std::unordered_set<std::uint32_t> reachable;
    while(!pending.empty()) {
        const auto id=pending.back();pending.pop_back();if(!reachable.insert(id).second)continue;
        for(const auto source:dependencies[id])pending.push_back(source);
    }
    for(const auto& n:valid.nodes)if(reachable.contains(n.id)) {
        degrees[n.id]=static_cast<int>(dependencies[n.id].size());if(degrees[n.id]==0)pending.push_back(n.id);
    }
    // Kahn ordering avoids recursive stack growth even for the full 2048-node chain.
    for(size_t head=0;head<pending.size();++head) {
        const auto id=pending[head];const auto* n=nodes.at(id);
        GraphInstruction instruction;instruction.node=*n;instruction.sources.fill(-1);instruction.sourceSlots.fill(0);
        const auto pins=NodePins(n->kind);
        for(size_t s=0;s<pins.size();++s) {
            if(!pins[s].input && pins[s].type!=PinType::Texture && pins[s].type!=PinType::Surface)instruction.numericOutputs|=static_cast<std::uint16_t>(1U<<s);
            if(pins[s].input && inputs.contains(PinId(id,s))) {
                const auto& l=inputs.at(PinId(id,s));instruction.sources[s]=indices.at(l.output/16);instruction.sourceSlots[s]=static_cast<int>(l.output%16)-1;
            }
        }
        const auto missing=[&](int slot){return instruction.sources[slot]<0;};
        if(n->kind==NodeKind::WorldPosition || n->kind==NodeKind::WorldNormal || (n->kind==NodeKind::Fresnel && (missing(0)||missing(1))) || (n->kind==NodeKind::HeightMask && missing(0)) || (n->kind==NodeKind::SlopeMask && missing(0)) || (n->kind==NodeKind::Triplanar && (missing(1)||missing(2))) || (n->kind==NodeKind::Noise && missing(0) && n->settings[0]!=0))draft.worldDependent_=true;
        if(n->kind==NodeKind::AO || n->kind==NodeKind::Curvature || n->kind==NodeKind::Cavity){draft.surfaceData_=true;draft.worldDependent_=true;}
        indices[id]=static_cast<int>(draft.instructions_.size());draft.instructions_.push_back(std::move(instruction));
        for(const auto next:outgoing[id])if(reachable.contains(next) && --degrees[next]==0)pending.push_back(next);
    }
    if(draft.instructions_.size()!=reachable.size()){error="Cyclic graph";return false;}
    if(output!=valid.nodes.end())draft.output_=indices.at(output->id);
    if(previewPin)draft.preview_=indices.at(previewPin/16);
    for (const auto& i : draft.instructions_) {
        const auto& n = i.node;
        if (n.kind == NodeKind::TextureParameter || ((n.kind == NodeKind::TextureSample || n.kind == NodeKind::Triplanar) && i.sources[0] < 0)) {
            if (!n.resource.empty() || !n.assetGuid.empty() || !n.assetId.empty() || n.kind == NodeKind::TextureParameter)
                draft.layout_.textures.push_back({n.id, static_cast<std::uint32_t>(draft.layout_.textures.size()), n.resource, n.assetGuid, n.assetId, n.parameter, n.value});
        }
        if (!n.assetGuid.empty()) draft.layout_.dependencyGuids.push_back(n.assetGuid);
    }
    auto& dependenciesGuids = draft.layout_.dependencyGuids;
    std::sort(dependenciesGuids.begin(), dependenciesGuids.end());
    dependenciesGuids.erase(std::unique(dependenciesGuids.begin(), dependenciesGuids.end()), dependenciesGuids.end());
    draft.layout_.worldDependent = draft.worldDependent_; draft.layout_.needsGeometryData = draft.surfaceData_;
    *this=std::move(draft);return true;
}
bool MaterialGraphProgram::ValidateOverrides(const ParameterOverrides& parameters, std::string& error) const
{
    error.clear();
    const auto find = [&](const std::string& name) -> const MaterialParameter* {
        const auto it = std::find_if(layout_.parameters.begin(), layout_.parameters.end(), [&](const auto& p) { return p.name == name; });
        return it == layout_.parameters.end() ? nullptr : &*it;
    };
    for (const auto& [name, value] : parameters.values) {
        const auto* p = find(name);
        if (!p || p->type == PinType::Texture) { error = "Missing numeric material parameter: " + name; return false; }
        for (float f : value) if (!std::isfinite(f) || std::abs(f) > 1000000) { error = "Invalid material parameter value: " + name; return false; }
    }
    for (const auto& [name, resource] : parameters.textures) {
        const auto* p = find(name);
        if (!p || p->type != PinType::Texture) { error = "Missing texture material parameter: " + name; return false; }
        if (resource.empty() || resource.size() > 4096) { error = "Invalid texture override: " + name; return false; }
    }
    return true;
}
bool MaterialGraphProgram::BuildParameterBuffer(const ParameterOverrides& parameters, std::vector<Float4>& buffer, std::string& error) const
{
    if (!ValidateOverrides(parameters, error)) return false;
    std::vector<Float4> draft(layout_.parameterBufferBytes / sizeof(Float4));
    for (const auto& p : layout_.parameters) if (p.type != PinType::Texture) {
        const auto it = parameters.values.find(p.name);
        draft[p.byteOffset / sizeof(Float4)] = it == parameters.values.end() ? p.defaultValue : it->second;
    }
    buffer = std::move(draft); return true;
}
Surface MaterialGraphProgram::Evaluate(const EvaluationContext& c, const ParameterOverrides& parameters, const TextureSampler& sampler, std::vector<std::array<GraphValue,14>>& work) const
{
    work.resize(instructions_.size());
    for(size_t index=0;index<instructions_.size();++index) {
        const auto& i=instructions_[index]; const auto& n=i.node; auto& out=work[index];
        for(auto mask=i.numericOutputs;mask;mask=static_cast<std::uint16_t>(mask&(mask-1)))out[std::countr_zero(mask)].number={};
        const auto linked=[&](int slot){return i.sources[slot]>=0;};
        const auto value=[&](int slot,Float4 fallback=Float4{})->Float4 {return linked(slot)?work[i.sources[slot]][i.sourceSlots[slot]].number:fallback;};
        const auto scalar=[&](int slot,float fallback)->float {return value(slot,{fallback,0,0,0})[0];};
        const auto surface=[&](int slot)->Surface {return linked(slot)?work[i.sources[slot]][i.sourceSlots[slot]].surface:Surface{};};
        const auto sourceTexture=[&]()->const GraphNode& {
            if(linked(0))return instructions_[i.sources[0]].node;
            return n;
        };
        const auto sample=[&](float u,float v)->SampledTexture {
            const auto& source=sourceTexture(); static const std::string empty;
            const auto& overridePath=parameters.textures.contains(source.parameter)?parameters.textures.at(source.parameter):empty;
            return sampler?sampler(source,overridePath,u,v):SampledTexture{source.value};
        };
        const auto sampledOutputs=[&](Float4 raw,int first,bool linearRgb){
            auto rgb=raw; if(n.srgb && !linearRgb)for(int k=0;k<3;++k)rgb[k]=SrgbToLinear(rgb[k]); out[first].number=rgb;
            for(int k=0;k<4;++k)out[first+1+k].number[0]=raw[k];
        };
        switch(n.kind) {
        case NodeKind::Scalar: case NodeKind::Color: case NodeKind::Vector2: case NodeKind::Vector3: out[0].number=n.value; break;
        case NodeKind::ScalarParameter: case NodeKind::ColorParameter: case NodeKind::Vector2Parameter: case NodeKind::Vector3Parameter: out[0].number=parameters.values.contains(n.parameter)?parameters.values.at(n.parameter):n.value;break;
        case NodeKind::TextureParameter: out[0].textureNode=n.id;break;
        case NodeKind::UV: out[0].number=c.uv;break;
        case NodeKind::WorldPosition:out[0].number=c.worldPosition;break;
        case NodeKind::WorldNormal:out[0].number=Normalize3(c.worldNormal,{0,1,0,0});break;
        case NodeKind::TilingOffset: {auto uv=value(0,c.uv),tiling=value(1,{n.value[0],n.value[1],0,0}),offset=value(2,{n.value[2],n.value[3],0,0});out[3].number={uv[0]*tiling[0]+offset[0],uv[1]*tiling[1]+offset[1],0,0};break;}
        case NodeKind::TextureSample: {auto uv=value(1,c.uv);auto raw=sample(uv[0],uv[1]);sampledOutputs(raw.rgba,2,raw.linearRgb);out[7].number=Normalize3({raw.rgba[0]*2-1,raw.rgba[1]*2-1,raw.rgba[2]*2-1,0});break;}
        case NodeKind::Triplanar: {auto p=value(1,c.worldPosition),normal=Normalize3(value(2,c.worldNormal),{0,1,0,0});const float scale=scalar(3,n.value[0]),sharpness=std::clamp(n.value[1],1.0f,32.0f);Float4 weights{};float sum=0;
            for(int k=0;k<3;++k){p[k]*=scale;weights[k]=std::pow(std::abs(normal[k]),sharpness);sum+=weights[k];}
            const auto x=sample(p[2],p[1]),y=sample(p[0],p[2]),z=sample(p[0],p[1]);Float4 raw{};
            for(int k=0;k<4;++k)raw[k]=(x.rgba[k]*weights[0]+y.rgba[k]*weights[1]+z.rgba[k]*weights[2])/std::max(sum,1e-6f);sampledOutputs(raw,4,x.linearRgb);break;}
        case NodeKind::Add: case NodeKind::Subtract: case NodeKind::Multiply: case NodeKind::Divide: case NodeKind::Minimum: case NodeKind::Maximum: case NodeKind::Power: {
            const float a=scalar(0,n.value[0]),b=scalar(1,n.value[1]);float result=0;
            switch(n.kind){case NodeKind::Add:result=a+b;break;case NodeKind::Subtract:result=a-b;break;case NodeKind::Multiply:result=a*b;break;case NodeKind::Divide:result=SafeDivide(a,b);break;case NodeKind::Minimum:result=std::min(a,b);break;case NodeKind::Maximum:result=std::max(a,b);break;default:result=std::pow(std::max(a,0.0f),std::clamp(b,-32.0f,32.0f));break;} out[2].number[0]=result;break;}
        case NodeKind::Lerp: {const float a=scalar(0,n.value[0]),b=scalar(1,n.value[1]),t=scalar(2,n.value[2]);out[3].number[0]=a+(b-a)*t;break;}
        case NodeKind::Clamp: case NodeKind::Smoothstep: {const float a=scalar(1,n.value[1]),b=scalar(2,n.value[2]),x=scalar(0,n.value[0]);out[3].number[0]=n.kind==NodeKind::Clamp?std::clamp(x,std::min(a,b),std::max(a,b)):Smooth(a,b,x);break;}
        case NodeKind::Remap: {const float x=scalar(0,n.settings[0]),a=scalar(1,n.value[0]),b=scalar(2,n.value[1]),u=scalar(3,n.value[2]),v=scalar(4,n.value[3]);out[5].number[0]=u+SafeDivide(x-a,b-a)*(v-u);break;}
        case NodeKind::Saturate: case NodeKind::OneMinus: case NodeKind::Abs: {const float x=scalar(0,n.value[0]);out[1].number[0]=n.kind==NodeKind::Saturate?Sat(x):(n.kind==NodeKind::OneMinus?1-x:std::abs(x));break;}
        case NodeKind::ColorMultiply: case NodeKind::ColorAdd: out[2].number=Math(value(0,n.value),value(1,{1,1,1,1}),n.kind==NodeKind::ColorMultiply);break;
        case NodeKind::Blend:out[3].number=Mix(value(0,n.value),value(1,{1,1,1,1}),scalar(2,n.settings[0]));break;
        case NodeKind::MakeColor:for(int k=0;k<4;++k)out[4].number[k]=scalar(k,n.value[k]);break;
        case NodeKind::SplitColor:for(int k=0;k<4;++k)out[k+1].number[0]=value(0,n.value)[k];break;
        case NodeKind::MakeVector:for(int k=0;k<3;++k)out[3].number[k]=scalar(k,n.value[k]);break;
        case NodeKind::SplitVector:for(int k=0;k<3;++k)out[k+1].number[0]=value(0,n.value)[k];break;
        case NodeKind::VectorScale:{auto v=value(0,n.value);const float s=scalar(1,n.settings[0]);for(int k=0;k<3;++k)v[k]*=s;out[2].number=v;break;}
        case NodeKind::Dot:out[2].number[0]=Dot3(value(0,n.value),value(1,{0,1,0,0}));break;
        case NodeKind::NormalBlend:out[3].number=NormalDetail(value(0,{0,0,1,0}),value(1,{0,0,1,0}),scalar(2,n.value[0]));break;
        case NodeKind::NormalStrength: {auto normal=Normalize3(value(0,{0,0,1,0}));const float strength=std::max(0.0f,scalar(1,n.value[0]));out[2].number=strength==0?Float4{0,0,1,0}:Normalize3({normal[0]*strength,normal[1]*strength,normal[2],0});break;}
        case NodeKind::NormalUnpack: {const auto encoded=value(0,n.value);out[1].number=Normalize3({encoded[0]*2-1,(encoded[1]*2-1)*(n.settings[0]==1?-1.0f:1.0f),encoded[2]*2-1,0});break;}
        case NodeKind::NormalFlipY: {auto normal=value(0,n.value);normal[1]=-normal[1];out[1].number=Normalize3(normal);break;}
        case NodeKind::Normalize: out[1].number=Normalize3(value(0,n.value));break;
        case NodeKind::Length: out[1].number[0]=std::sqrt(Dot3(value(0,n.value),value(0,n.value)));break;
        case NodeKind::Cross: {const auto a=value(0,n.value),b=value(1,{0,1,0,0});out[2].number={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],0};break;}
        case NodeKind::Distance: {const auto a=value(0,n.value),b=value(1,{0,1,0,0});Float4 delta{a[0]-b[0],a[1]-b[1],a[2]-b[2],0};out[2].number[0]=std::sqrt(Dot3(delta,delta));break;}
        case NodeKind::Reflect: {const auto a=value(0,n.value),normal=Normalize3(value(1,{0,1,0,0}),{0,1,0,0});const float dot=2*Dot3(a,normal);out[2].number={a[0]-dot*normal[0],a[1]-dot*normal[1],a[2]-dot*normal[2],0};break;}
        case NodeKind::Sine: case NodeKind::Cosine: case NodeKind::Fract: case NodeKind::Floor: case NodeKind::Ceil: case NodeKind::Sign: case NodeKind::MaskInvert: {
            const float x=scalar(0,n.value[0]);float result=0;
            switch(n.kind) { case NodeKind::Sine:result=std::sin(x);break;case NodeKind::Cosine:result=std::cos(x);break;case NodeKind::Fract:result=x-std::floor(x);break;case NodeKind::Floor:result=std::floor(x);break;case NodeKind::Ceil:result=std::ceil(x);break;case NodeKind::Sign:result=x<0?-1.0f:(x>0?1.0f:0.0f);break;default:result=1-Sat(x);break; }
            out[1].number[0]=result;break;}
        case NodeKind::Step: out[2].number[0]=scalar(1,n.value[1])>=scalar(0,n.value[0])?1.0f:0.0f;break;
        case NodeKind::MaskCombine: {const float a=scalar(0,n.value[0]),b=scalar(1,n.value[1]);float result=0;
            switch(static_cast<int>(std::clamp(n.settings[0],0.0f,4.0f))) {case 0:result=a*b;break;case 1:result=a+b;break;case 2:result=std::min(a,b);break;case 3:result=std::max(a,b);break;default:result=a-b;break;}out[2].number[0]=Sat(result);break;}
        case NodeKind::MaskLevels: {const float x=scalar(0,n.value[0]),black=scalar(1,n.value[1]),white=scalar(2,n.value[2]),gamma=std::max(.001f,scalar(3,n.value[3]));out[4].number[0]=std::pow(Sat(SafeDivide(x-black,white-black)),1/gamma);break;}
        case NodeKind::MaskContrast:out[2].number[0]=Sat((scalar(0,n.value[0])-.5f)*std::max(0.0f,scalar(1,n.value[1]))+.5f);break;
        case NodeKind::Fresnel:out[3].number[0]=std::pow(1-Sat(Dot3(Normalize3(value(0,c.worldNormal),{0,1,0,0}),Normalize3(value(1,c.viewDirection)))),std::clamp(scalar(2,n.value[0]),.001f,64.0f));break;
        case NodeKind::Noise: {auto p=value(0,n.settings[0]==0?Float4{c.uv[0],c.uv[1],0,0}:c.worldPosition);const float scale=scalar(1,n.value[0]);for(int k=0;k<3;++k)p[k]*=scale;out[2].number[0]=Noise(p,std::clamp(static_cast<int>(n.value[1]),1,8),Sat(n.value[2]),static_cast<std::uint32_t>(static_cast<int>(scalar(3,n.value[3]))));break;}
        case NodeKind::HeightMask:out[3].number[0]=Smooth(scalar(1,n.value[0]),scalar(2,n.value[1]),value(0,c.worldPosition)[1]);break;
        case NodeKind::SlopeMask:{const auto normal=Normalize3(value(0,c.worldNormal),{0,1,0,0});const float angle=std::acos(std::clamp(normal[1],-1.0f,1.0f))*57.2957795f;out[3].number[0]=Smooth(scalar(1,n.value[0]),scalar(2,n.value[1]),angle);break;}
        case NodeKind::AO:out[0].number[0]=c.ao;break;
        case NodeKind::Curvature:out[0].number[0]=c.curvature;break;
        case NodeKind::Cavity:out[0].number[0]=c.cavity;break;
        case NodeKind::PbrOutput: case NodeKind::MaterialLayer: {
            Surface s=n.kind==NodeKind::PbrOutput && linked(8)?surface(8):Surface{};
            if(n.kind==NodeKind::MaterialLayer || !linked(8)){s.color=n.value;s.roughness=n.settings[0];s.metallic=n.settings[1];s.ao=n.settings[2];s.height=n.settings[3];s.opacity=n.value[3];}
            if(linked(0))s.color=value(0);if(linked(1))s.roughness=scalar(1,0);if(linked(2))s.metallic=scalar(2,0);if(linked(3))s.emissive=value(3);if(linked(4))s.normal=Normalize3(value(4));if(linked(5))s.ao=scalar(5,1);if(linked(6))s.height=scalar(6,0);if(linked(7))s.opacity=scalar(7,1);
            s.roughness=Sat(s.roughness);s.metallic=Sat(s.metallic);s.ao=Sat(s.ao);s.opacity=Sat(s.opacity);out[n.kind==NodeKind::PbrOutput?0:8].surface=s;break;}
        case NodeKind::LayerBlend:case NodeKind::HeightBlend:{auto a=surface(0),b=surface(1);float t=Sat(scalar(2,n.value[0]));if(n.kind==NodeKind::HeightBlend && t>0 && t<1)t=Sat(0.5f+(b.height-a.height+2*t-1)/(2*std::max(scalar(3,n.value[1]),0.001f)));out[n.kind==NodeKind::HeightBlend?4:3].surface=MixSurface(a,b,t,n.settings[3]==1?static_cast<unsigned>(n.settings[2]):255);break;}
        default:break;
        }
        // Protect downstream nodes and texture samplers from overflow/invalid arithmetic.
        for(auto mask=i.numericOutputs;mask;mask=static_cast<std::uint16_t>(mask&(mask-1)))for(auto& f:out[std::countr_zero(mask)].number)if(!std::isfinite(f))f=0;else f=std::clamp(f,-1000000.0f,1000000.0f);
    }
    return output_>=0?work[output_][0].surface:Surface{};
}
GraphValue MaterialGraphProgram::PreviewValue(const std::vector<std::array<GraphValue,14>>& w) const {return preview_>=0 && w.size()==instructions_.size()?w[preview_][previewSlot_]:GraphValue{};}

std::string MaterialGraphProgram::GenerateHlsl() const
{
    std::ostringstream h; h.imbue(std::locale::classic());
    h << R"HLSL(// Generated material graph. Texture bindings use UNORM views; RGB sRGB decoding is explicit.
// Supply geometry AO/curvature/cavity when used. Normal outputs are tangent-space.
struct GraphContext { float2 uv; float3 worldPosition; float3 worldNormal; float3 viewDirection; float ao; float curvature; float cavity; };
struct GraphSurface { float4 color; float3 normal; float3 emissive; float roughness; float metallic; float ao; float height; float opacity; };
GraphSurface GraphDefault() { GraphSurface s; s.color=float4(.5,.5,.5,1); s.normal=float3(0,0,1); s.emissive=0; s.roughness=.5; s.metallic=0; s.ao=1; s.height=0; s.opacity=1; return s; }
float GraphDiv(float a,float b) { return abs(b)<1e-6 ? 0 : a/b; }
float GraphSmooth(float a,float b,float x) { float t=abs(b-a)<1e-6 ? (x>=b?1:0) : saturate((x-a)/(b-a)); return t*t*(3-2*t); }
float3 GraphNormalize(float3 x) { return dot(x,x)<1e-12 ? float3(0,0,1) : normalize(x); }
float3 GraphWorldNormal(float3 x) { return dot(x,x)<1e-12 ? float3(0,1,0) : normalize(x); }
float3 GraphNormalStrength(float3 x,float strength) { x=GraphNormalize(x); strength=max(strength,0); return strength==0 ? float3(0,0,1) : GraphNormalize(float3(x.xy*strength,x.z)); }
float GraphLinear(float x) { return x<=.04045 ? x/12.92 : pow((x+.055)/1.055,2.4); }
float3 GraphSRGB(float3 x) { return float3(GraphLinear(x.r),GraphLinear(x.g),GraphLinear(x.b)); }
float3 GraphNormalBlend(float3 a,float3 b,float strength) { a=GraphNormalize(a); b=GraphNormalize(lerp(float3(0,0,1),GraphNormalize(b),saturate(strength))); float3 t=a+float3(0,0,1),u=b*float3(-1,-1,1); return abs(t.z)<1e-6 ? a : GraphNormalize(t*dot(t,u)/t.z-u); }
GraphSurface GraphBlend(GraphSurface a,GraphSurface b,float t) { t=saturate(t); a.color=lerp(a.color,b.color,t); a.normal=GraphNormalize(lerp(a.normal,b.normal,t)); a.emissive=lerp(a.emissive,b.emissive,t); a.roughness=lerp(a.roughness,b.roughness,t); a.metallic=lerp(a.metallic,b.metallic,t); a.ao=lerp(a.ao,b.ao,t); a.height=lerp(a.height,b.height,t); a.opacity=lerp(a.opacity,b.opacity,t); return a; }
uint GraphHash(uint x) { x^=x>>16; x*=0x7feb352dU; x^=x>>15; x*=0x846ca68bU; return x^(x>>16); }
float GraphLattice(int3 p,uint seed) { return float(GraphHash(uint(p.x)*73856093U ^ uint(p.y)*19349663U ^ uint(p.z)*83492791U ^ seed)&0x00ffffffU)/16777215.0; }
float GraphNoise(float3 p,int octaves,float persistence,uint seed) { float result=0,weight=0,amplitude=1;
    for(int o=0;o<octaves;++o) { float3 q=fmod(p,4096.0),v=frac(q); int3 cell=int3(floor(q)); v=v*v*(3-2*v);
        float n=lerp(lerp(lerp(GraphLattice(cell,seed),GraphLattice(cell+int3(1,0,0),seed),v.x),lerp(GraphLattice(cell+int3(0,1,0),seed),GraphLattice(cell+int3(1,1,0),seed),v.x),v.y),
          lerp(lerp(GraphLattice(cell+int3(0,0,1),seed),GraphLattice(cell+int3(1,0,1),seed),v.x),lerp(GraphLattice(cell+int3(0,1,1),seed),GraphLattice(cell+int3(1,1,1),seed),v.x),v.y),v.z);
        result+=n*amplitude; weight+=amplitude; amplitude*=persistence; p*=2; } return result/max(weight,1e-6); }
float4 GraphFinite(float4 v) { return float4(isfinite(v.x)?clamp(v.x,-1e6,1e6):0,isfinite(v.y)?clamp(v.y,-1e6,1e6):0,isfinite(v.z)?clamp(v.z,-1e6,1e6):0,isfinite(v.w)?clamp(v.w,-1e6,1e6):0); }
)HLSL";
    const bool anyParameter = layout_.parameterBufferBytes != 0;
    if(anyParameter){ h<<"cbuffer GraphParameters : register(b0) {\n";
        for (const auto& p : layout_.parameters) if (p.type != PinType::Texture) h << "    float4 parameter_" << p.nodeId << "; // offset " << p.byteOffset << ", default " << Literal(p.defaultValue) << "\n";
        h<<"};\n";
    }
    std::unordered_set<std::uint32_t> textures;
    for(const auto& i:instructions_)if(i.node.kind==NodeKind::TextureParameter || ((i.node.kind==NodeKind::TextureSample || i.node.kind==NodeKind::Triplanar)&&i.sources[0]<0)) {
        if(!i.node.resource.empty() || !i.node.assetGuid.empty() || !i.node.assetId.empty() || i.node.kind==NodeKind::TextureParameter)textures.insert(i.node.id);
    }
    for (const auto& t : layout_.textures) h << "Texture2D<float4> texture_" << t.nodeId << " : register(t" << t.binding << ");\n";
    if(!textures.empty()) {
        h<<"SamplerState graphSampler : register(s0); // bilinear, repeat\n";
        h<<"cbuffer GraphTextureProperties : register(b1) {\n";
        for(const auto& i:instructions_)if(textures.contains(i.node.id))h<<"    float4 textureInfo_"<<i.node.id<<"; // x=1 for linear RGB sources (EXR/HDR), x=0 for encoded RGB; other fields reserved\n";
        h<<"};\n";
    }
    h<<"GraphSurface EvaluateMaterialGraph(GraphContext c) {\n";
    std::unordered_map<std::uint32_t, const MaterialNodeOrigin*> origins;
    for (const auto& origin : layout_.subgraphOrigins) origins[origin.expandedNode] = &origin;
    const auto var=[&](int index,int slot){return "n"+std::to_string(instructions_[index].node.id)+"_"+std::to_string(slot);};
    for(size_t index=0;index<instructions_.size();++index) {
        const auto& i=instructions_[index];const auto& n=i.node; const auto prefix="n"+std::to_string(n.id);
        const auto linked=[&](int s){return i.sources[s]>=0;};
        const auto value=[&](int s,Float4 fallback=Float4{}) {return linked(s)?var(i.sources[s],i.sourceSlots[s]):Literal(fallback);};
        const auto scalar=[&](int s,float fallback) {return linked(s)?"("+var(i.sources[s],i.sourceSlots[s])+".x)":Float(fallback);};
        const auto context=[&](int s,const char* fallback){return linked(s)?var(i.sources[s],i.sourceSlots[s]):std::string(fallback);};
        const auto emit=[&](int slot,const std::string& expression){h<<"    float4 "<<var(static_cast<int>(index),slot)<<" = GraphFinite("<<expression<<");\n";};
        const auto emitScalar=[&](int slot,const std::string& expression){emit(slot,"float4("+expression+",0,0,0)");};
        const auto surface=[&](int s){return linked(s)?var(i.sources[s],i.sourceSlots[s]):std::string("GraphDefault()");};
        const auto sample=[&](std::string uv){const auto id=linked(0)?instructions_[i.sources[0]].node.id:n.id;return textures.contains(id)?"texture_"+std::to_string(id)+".Sample(graphSampler,"+uv+")":"float4(1,1,1,1)";};
        const auto emitSample=[&](int first,const std::string& raw){const auto sourceId=linked(0)?instructions_[i.sources[0]].node.id:n.id;
            const auto rgb=n.srgb?"float4(GraphSRGB("+raw+".rgb),"+raw+".a)":raw;
            emit(first,n.srgb && textures.contains(sourceId)?"(textureInfo_"+std::to_string(sourceId)+".x>0.5?"+raw+":"+rgb+")":rgb);for(int k=0;k<4;++k)emitScalar(first+k+1,raw+"."+std::string(1,"rgba"[k]));};
        h<<"    // Node "<<n.id<<": "<<NodeKindName(n.kind)<<"\n";
        if (const auto found=origins.find(n.id); found!=origins.end()) {
            const auto& origin=*found->second;
            h<<"    // Subgraph "<<origin.assetGuid<<", source node "<<origin.sourceNode<<", scope "<<origin.instancePath<<"\n";
        }
        switch(n.kind) {
        case NodeKind::Scalar: case NodeKind::Color: case NodeKind::Vector2: case NodeKind::Vector3:emit(0,Literal(n.value));break;
        case NodeKind::ScalarParameter:case NodeKind::ColorParameter:case NodeKind::Vector2Parameter:case NodeKind::Vector3Parameter:emit(0,"parameter_"+std::to_string(n.id));break;
        case NodeKind::TextureParameter:break;
        case NodeKind::UV:emit(0,"float4(c.uv,0,0)");break;
        case NodeKind::WorldPosition:emit(0,"float4(c.worldPosition,0)");break;
        case NodeKind::WorldNormal:emit(0,"float4(GraphWorldNormal(c.worldNormal),0)");break;
        case NodeKind::TilingOffset:emit(3,"float4("+context(0,"float4(c.uv,0,0)")+".xy*"+value(1,{n.value[0],n.value[1],0,0})+".xy+"+value(2,{n.value[2],n.value[3],0,0})+".xy,0,0)");break;
        case NodeKind::TextureSample:{const auto raw=prefix+"_raw";h<<"    float4 "<<raw<<" = "<<sample(context(1,"float4(c.uv,0,0)")+".xy")<<";\n";emitSample(2,raw);emit(7,"float4(GraphNormalize("+raw+".rgb*2-1),0)");break;}
        case NodeKind::Triplanar:{const auto p=prefix+"_p",w=prefix+"_w",raw=prefix+"_raw";
            h<<"    float3 "<<p<<" = "<<context(1,"float4(c.worldPosition,0)")<<".xyz*"<<scalar(3,n.value[0])<<";\n";
            h<<"    float3 "<<w<<" = pow(abs(GraphWorldNormal("<<context(2,"float4(c.worldNormal,0)")<<".xyz)),"<<Float(std::clamp(n.value[1],1.0f,32.0f))<<"); "<<w<<" /= max(dot("<<w<<",float3(1,1,1)),1e-6);\n";
            h<<"    float4 "<<raw<<" = "<<sample(p+".zy")<<"*"<<w<<".x + "<<sample(p+".xz")<<"*"<<w<<".y + "<<sample(p+".xy")<<"*"<<w<<".z;\n";emitSample(4,raw);break;}
        case NodeKind::Add:case NodeKind::Subtract:case NodeKind::Multiply:case NodeKind::Divide:case NodeKind::Minimum:case NodeKind::Maximum:case NodeKind::Power:{const auto a=scalar(0,n.value[0]),b=scalar(1,n.value[1]);std::string e;
            switch(n.kind){case NodeKind::Add:e=a+"+"+b;break;case NodeKind::Subtract:e=a+"-"+b;break;case NodeKind::Multiply:e=a+"*"+b;break;case NodeKind::Divide:e="GraphDiv("+a+","+b+")";break;case NodeKind::Minimum:e="min("+a+","+b+")";break;case NodeKind::Maximum:e="max("+a+","+b+")";break;default:e="pow(max("+a+",0),clamp("+b+",-32,32))";break;}emitScalar(2,e);break;}
        case NodeKind::Lerp:emitScalar(3,"lerp("+scalar(0,n.value[0])+","+scalar(1,n.value[1])+","+scalar(2,n.value[2])+")");break;
        case NodeKind::Clamp:case NodeKind::Smoothstep:{const auto a=scalar(1,n.value[1]),b=scalar(2,n.value[2]),x=scalar(0,n.value[0]);emitScalar(3,n.kind==NodeKind::Clamp?"clamp("+x+",min("+a+","+b+"),max("+a+","+b+"))":"GraphSmooth("+a+","+b+","+x+")");break;}
        case NodeKind::Remap:{const auto x=scalar(0,n.settings[0]),a=scalar(1,n.value[0]),b=scalar(2,n.value[1]),u=scalar(3,n.value[2]),v=scalar(4,n.value[3]);emitScalar(5,u+"+GraphDiv("+x+"-"+a+","+b+"-"+a+")*("+v+"-"+u+")");break;}
        case NodeKind::Saturate:case NodeKind::OneMinus:case NodeKind::Abs:{const auto x=scalar(0,n.value[0]);emitScalar(1,n.kind==NodeKind::Saturate?"saturate("+x+")":(n.kind==NodeKind::OneMinus?"1-"+x:"abs("+x+")"));break;}
        case NodeKind::ColorMultiply:case NodeKind::ColorAdd:emit(2,value(0,n.value)+(n.kind==NodeKind::ColorMultiply?"*":"+")+value(1,{1,1,1,1}));break;
        case NodeKind::Blend:emit(3,"lerp("+value(0,n.value)+","+value(1,{1,1,1,1})+","+scalar(2,n.settings[0])+")");break;
        case NodeKind::MakeColor:emit(4,"float4("+scalar(0,n.value[0])+","+scalar(1,n.value[1])+","+scalar(2,n.value[2])+","+scalar(3,n.value[3])+")");break;
        case NodeKind::SplitColor:for(int k=0;k<4;++k)emitScalar(k+1,value(0,n.value)+"."+std::string(1,"rgba"[k]));break;
        case NodeKind::MakeVector:emit(3,"float4("+scalar(0,n.value[0])+","+scalar(1,n.value[1])+","+scalar(2,n.value[2])+",0)");break;
        case NodeKind::SplitVector:for(int k=0;k<3;++k)emitScalar(k+1,value(0,n.value)+"."+std::string(1,"xyz"[k]));break;
        case NodeKind::VectorScale:emit(2,value(0,n.value)+"*"+scalar(1,n.settings[0]));break;
        case NodeKind::Dot:emitScalar(2,"dot("+value(0,n.value)+".xyz,"+value(1,{0,1,0,0})+".xyz)");break;
        case NodeKind::NormalBlend:emit(3,"float4(GraphNormalBlend("+value(0,{0,0,1,0})+".xyz,"+value(1,{0,0,1,0})+".xyz,"+scalar(2,n.value[0])+"),0)");break;
        case NodeKind::NormalStrength:emit(2,"float4(GraphNormalStrength("+value(0,{0,0,1,0})+".xyz,"+scalar(1,n.value[0])+"),0)");break;
        case NodeKind::NormalUnpack:emit(1,"float4(GraphNormalize(("+value(0,n.value)+".xyz*2-1)*float3(1,"+(n.settings[0]==1?std::string("-1"):std::string("1"))+",1)),0)");break;
        case NodeKind::NormalFlipY:emit(1,"float4(GraphNormalize("+value(0,n.value)+".xyz*float3(1,-1,1)),0)");break;
        case NodeKind::Normalize:emit(1,"float4(GraphNormalize("+value(0,n.value)+".xyz),0)");break;
        case NodeKind::Length:emitScalar(1,"length("+value(0,n.value)+".xyz)");break;
        case NodeKind::Cross:emit(2,"float4(cross("+value(0,n.value)+".xyz,"+value(1,{0,1,0,0})+".xyz),0)");break;
        case NodeKind::Distance:emitScalar(2,"distance("+value(0,n.value)+".xyz,"+value(1,{0,1,0,0})+".xyz)");break;
        case NodeKind::Reflect:emit(2,"float4(reflect("+value(0,n.value)+".xyz,GraphWorldNormal("+value(1,{0,1,0,0})+".xyz)),0)");break;
        case NodeKind::Sine:case NodeKind::Cosine:case NodeKind::Fract:case NodeKind::Floor:case NodeKind::Ceil:case NodeKind::Sign: {
            const char* op=n.kind==NodeKind::Sine?"sin":(n.kind==NodeKind::Cosine?"cos":(n.kind==NodeKind::Fract?"frac":(n.kind==NodeKind::Floor?"floor":(n.kind==NodeKind::Ceil?"ceil":"sign"))));emitScalar(1,std::string(op)+"("+scalar(0,n.value[0])+")");break;}
        case NodeKind::MaskInvert:emitScalar(1,"1-saturate("+scalar(0,n.value[0])+")");break;
        case NodeKind::Step:emitScalar(2,"step("+scalar(0,n.value[0])+","+scalar(1,n.value[1])+")");break;
        case NodeKind::MaskCombine: {const auto a=scalar(0,n.value[0]),b=scalar(1,n.value[1]);std::string result;
            switch(static_cast<int>(std::clamp(n.settings[0],0.0f,4.0f))) {case 0:result=a+"*"+b;break;case 1:result=a+"+"+b;break;case 2:result="min("+a+","+b+")";break;case 3:result="max("+a+","+b+")";break;default:result=a+"-"+b;break;}emitScalar(2,"saturate("+result+")");break;}
        case NodeKind::MaskLevels:emitScalar(4,"pow(saturate(GraphDiv("+scalar(0,n.value[0])+"-"+scalar(1,n.value[1])+","+scalar(2,n.value[2])+"-"+scalar(1,n.value[1])+")),1/max("+scalar(3,n.value[3])+",.001))");break;
        case NodeKind::MaskContrast:emitScalar(2,"saturate(("+scalar(0,n.value[0])+"-.5)*max("+scalar(1,n.value[1])+",0)+.5)");break;
        case NodeKind::Fresnel:emitScalar(3,"pow(1-saturate(dot(GraphWorldNormal("+context(0,"float4(c.worldNormal,0)")+".xyz),GraphNormalize("+context(1,"float4(c.viewDirection,0)")+".xyz))),clamp("+scalar(2,n.value[0])+",.001,64))");break;
        case NodeKind::Noise:emitScalar(2,"GraphNoise("+context(0,n.settings[0]==0?"float4(c.uv,0,0)":"float4(c.worldPosition,0)")+".xyz*"+scalar(1,n.value[0])+","+std::to_string(std::clamp(static_cast<int>(n.value[1]),1,8))+","+Float(Sat(n.value[2]))+",uint(int("+scalar(3,n.value[3])+")))");break;
        case NodeKind::HeightMask:emitScalar(3,"GraphSmooth("+scalar(1,n.value[0])+","+scalar(2,n.value[1])+","+context(0,"float4(c.worldPosition,0)")+".y)");break;
        case NodeKind::SlopeMask:emitScalar(3,"GraphSmooth("+scalar(1,n.value[0])+","+scalar(2,n.value[1])+",acos(clamp(GraphWorldNormal("+context(0,"float4(c.worldNormal,0)")+".xyz).y,-1,1))*57.2957795)");break;
        case NodeKind::AO:emitScalar(0,"c.ao");break;
        case NodeKind::Curvature:emitScalar(0,"c.curvature");break;
        case NodeKind::Cavity:emitScalar(0,"c.cavity");break;
        case NodeKind::PbrOutput:case NodeKind::MaterialLayer:{const auto target=var(static_cast<int>(index),n.kind==NodeKind::PbrOutput?0:8);h<<"    GraphSurface "<<target<<" = "<<(n.kind==NodeKind::PbrOutput&&linked(8)?surface(8):"GraphDefault()")<<";\n";
            if(n.kind==NodeKind::MaterialLayer || !linked(8))h<<"    "<<target<<".color="<<Literal(n.value)<<"; "<<target<<".roughness="<<Float(n.settings[0])<<"; "<<target<<".metallic="<<Float(n.settings[1])<<"; "<<target<<".ao="<<Float(n.settings[2])<<"; "<<target<<".height="<<Float(n.settings[3])<<"; "<<target<<".opacity="<<Float(n.value[3])<<";\n";
            const char* fields[]={"color","roughness","metallic","emissive","normal","ao","height","opacity"};
            for(int s=0;s<8;++s)if(linked(s)){h<<"    "<<target<<"."<<fields[s]<<" = "; if(s==4)h<<"GraphNormalize("<<value(s)<<".xyz)";else if(s==3)h<<value(s)<<".xyz";else if(s==0)h<<value(s);else h<<scalar(s,0);h<<";\n";}
            h<<"    "<<target<<".roughness=saturate("<<target<<".roughness); "<<target<<".metallic=saturate("<<target<<".metallic); "<<target<<".ao=saturate("<<target<<".ao); "<<target<<".opacity=saturate("<<target<<".opacity);\n";break;}
        case NodeKind::LayerBlend:case NodeKind::HeightBlend:{const auto t=prefix+"_factor",a=prefix+"_base",b=prefix+"_top";
            h<<"    GraphSurface "<<a<<"="<<surface(0)<<", "<<b<<"="<<surface(1)<<"; float "<<t<<"=saturate("<<scalar(2,n.value[0])<<");\n";
            if(n.kind==NodeKind::HeightBlend)h<<"    if("<<t<<">0 && "<<t<<"<1) "<<t<<"=saturate(.5+("<<b<<".height-"<<a<<".height+2*"<<t<<"-1)/(2*max("<<scalar(3,n.value[1])<<",.001)));\n";
            const auto target = var(static_cast<int>(index),n.kind==NodeKind::HeightBlend?4:3);
            h<<"    GraphSurface "<<target<<"=GraphBlend("<<a<<","<<b<<","<<t<<");\n";
            if (n.settings[3] == 1) {
                const char* fields[] = {"color", "roughness", "metallic", "emissive", "normal", "ao", "height", "opacity"};
                const auto channels = static_cast<unsigned>(n.settings[2]);
                for (unsigned k = 0; k < 8; ++k) if (!(channels & (1U << k))) h << "    " << target << "." << fields[k] << "=" << a << "." << fields[k] << ";\n";
            }
            break;}
        default:break;
        }
    }
    h<<"    return "<<(output_>=0?var(output_,0):"GraphDefault()")<<";\n}\n";
    h<<R"HLSL(// Standalone entry for shader validation and preview integration.
float4 GraphValidationPS(float2 uv:TEXCOORD0):SV_TARGET {
    GraphContext c; c.uv=uv; c.worldPosition=float3(uv.x,0,uv.y); c.worldNormal=float3(0,1,0); c.viewDirection=float3(0,0,1); c.ao=1; c.curvature=.5; c.cavity=0;
    GraphSurface s=EvaluateMaterialGraph(c); return float4(s.color.rgb+s.emissive,s.opacity);
}
)HLSL";return h.str();
}
}
