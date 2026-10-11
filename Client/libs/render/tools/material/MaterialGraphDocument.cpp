#include "MaterialGraphDocument.h"
#include <crude_json.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <unordered_map>

namespace material_tool
{
namespace
{
constexpr std::uint32_t MaxId = 0x00ffffff;
constexpr std::size_t MaxNodes = 2048, MaxLinks = 8192;
using P = PinType;
constexpr PinDefinition ScalarPins[] = {{"Value", P::Scalar, false}};
constexpr PinDefinition ColorPins[] = {{"RGB", P::Color, false}};
constexpr PinDefinition V2Pins[] = {{"UV", P::Vector2, false}};
constexpr PinDefinition V3Pins[] = {{"XYZ", P::Vector3, false}};
constexpr PinDefinition TexturePins[] = {{"Texture", P::Texture, false}};
constexpr PinDefinition MultiplyPins[] = {{"A", P::Scalar, true}, {"B", P::Scalar, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition ThreeScalarPins[] = {{"A", P::Scalar, true}, {"B", P::Scalar, true}, {"Factor", P::Scalar, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition ClampPins[] = {{"Value", P::Scalar, true}, {"Min", P::Scalar, true}, {"Max", P::Scalar, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition RemapPins[] = {{"Value", P::Scalar, true}, {"In Min", P::Scalar, true}, {"In Max", P::Scalar, true}, {"Out Min", P::Scalar, true}, {"Out Max", P::Scalar, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition UnaryPins[] = {{"Value", P::Scalar, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition ColorMathPins[] = {{"A", P::Color, true}, {"B", P::Color, true}, {"Result", P::Color, false}};
constexpr PinDefinition BlendPins[] = {{"A", P::Color, true}, {"B", P::Color, true}, {"Factor", P::Scalar, true}, {"Result", P::Color, false}};
constexpr PinDefinition TilingPins[] = {{"UV", P::Vector2, true}, {"Tiling", P::Vector2, true}, {"Offset", P::Vector2, true}, {"UV", P::Vector2, false}};
constexpr PinDefinition SamplePins[] = {{"Texture", P::Texture, true}, {"UV", P::Vector2, true}, {"RGB", P::Color, false}, {"R", P::Scalar, false}, {"G", P::Scalar, false}, {"B", P::Scalar, false}, {"A", P::Scalar, false}, {"Normal", P::Vector3, false}};
constexpr PinDefinition TriplanarPins[] = {{"Texture", P::Texture, true}, {"Position", P::Vector3, true}, {"Normal", P::Vector3, true}, {"Scale", P::Scalar, true}, {"RGB", P::Color, false}, {"R", P::Scalar, false}, {"G", P::Scalar, false}, {"B", P::Scalar, false}, {"A", P::Scalar, false}};
constexpr PinDefinition MakeColorPins[] = {{"R", P::Scalar, true}, {"G", P::Scalar, true}, {"B", P::Scalar, true}, {"A", P::Scalar, true}, {"RGB", P::Color, false}};
constexpr PinDefinition SplitColorPins[] = {{"RGB", P::Color, true}, {"R", P::Scalar, false}, {"G", P::Scalar, false}, {"B", P::Scalar, false}, {"A", P::Scalar, false}};
constexpr PinDefinition MakeVectorPins[] = {{"X", P::Scalar, true}, {"Y", P::Scalar, true}, {"Z", P::Scalar, true}, {"XYZ", P::Vector3, false}};
constexpr PinDefinition SplitVectorPins[] = {{"XYZ", P::Vector3, true}, {"X", P::Scalar, false}, {"Y", P::Scalar, false}, {"Z", P::Scalar, false}};
constexpr PinDefinition VectorScalePins[] = {{"XYZ", P::Vector3, true}, {"Scale", P::Scalar, true}, {"XYZ", P::Vector3, false}};
constexpr PinDefinition DotPins[] = {{"A", P::Vector3, true}, {"B", P::Vector3, true}, {"Result", P::Scalar, false}};
constexpr PinDefinition NormalBlendPins[] = {{"Base", P::Vector3, true}, {"Detail", P::Vector3, true}, {"Strength", P::Scalar, true}, {"Normal", P::Vector3, false}};
constexpr PinDefinition NoisePins[] = {{"Position", P::Vector3, true}, {"Scale", P::Scalar, true}, {"Mask", P::Scalar, false}, {"Seed", P::Scalar, true}};
constexpr PinDefinition NormalStrengthPins[] = {{"Normal", P::Vector3, true}, {"Strength", P::Scalar, true}, {"Normal", P::Vector3, false}};
constexpr PinDefinition VectorUnaryPins[] = {{"XYZ", P::Vector3, true}, {"XYZ", P::Vector3, false}};
constexpr PinDefinition VectorLengthPins[] = {{"XYZ", P::Vector3, true}, {"Length", P::Scalar, false}};
constexpr PinDefinition VectorBinaryPins[] = {{"A", P::Vector3, true}, {"B", P::Vector3, true}, {"XYZ", P::Vector3, false}};
constexpr PinDefinition UnpackNormalPins[] = {{"Encoded RGB", P::Color, true}, {"Normal", P::Vector3, false}};
constexpr PinDefinition LevelsPins[] = {{"Mask", P::Scalar, true}, {"Black", P::Scalar, true}, {"White", P::Scalar, true}, {"Gamma", P::Scalar, true}, {"Mask", P::Scalar, false}};
constexpr PinDefinition ContrastPins[] = {{"Mask", P::Scalar, true}, {"Contrast", P::Scalar, true}, {"Mask", P::Scalar, false}};
constexpr PinDefinition FresnelPins[] = {{"World Normal", P::Vector3, true}, {"World View Direction", P::Vector3, true}, {"Power", P::Scalar, true}, {"Mask", P::Scalar, false}};
constexpr PinDefinition HeightPins[] = {{"Position", P::Vector3, true}, {"Min", P::Scalar, true}, {"Max", P::Scalar, true}, {"Mask", P::Scalar, false}};
constexpr PinDefinition SlopePins[] = {{"Normal", P::Vector3, true}, {"Min Degrees", P::Scalar, true}, {"Max Degrees", P::Scalar, true}, {"Mask", P::Scalar, false}};
constexpr PinDefinition OutputPins[] = {{"Base Color", P::Color, true}, {"Roughness", P::Scalar, true}, {"Metallic", P::Scalar, true}, {"Emissive", P::Color, true}, {"Normal", P::Vector3, true}, {"AO", P::Scalar, true}, {"Height", P::Scalar, true}, {"Opacity", P::Scalar, true}, {"Surface", P::Surface, true}};
constexpr PinDefinition LayerPins[] = {{"Base Color", P::Color, true}, {"Roughness", P::Scalar, true}, {"Metallic", P::Scalar, true}, {"Emissive", P::Color, true}, {"Normal", P::Vector3, true}, {"AO", P::Scalar, true}, {"Height", P::Scalar, true}, {"Opacity", P::Scalar, true}, {"Surface", P::Surface, false}};
constexpr PinDefinition LayerBlendPins[] = {{"Base", P::Surface, true}, {"Top", P::Surface, true}, {"Mask", P::Scalar, true}, {"Surface", P::Surface, false}};
constexpr PinDefinition HeightBlendPins[] = {{"Base", P::Surface, true}, {"Top", P::Surface, true}, {"Mask", P::Scalar, true}, {"Width", P::Scalar, true}, {"Surface", P::Surface, false}};
constexpr const char* Names[] = {
    "Scalar", "Color", "Multiply", "Color Blend", "PBR Output",
    "Vector2", "Vector3", "Scalar Parameter", "Color Parameter", "Texture Parameter",
    "UV", "Tiling / Offset", "World Position", "World Normal", "Texture Sample", "Triplanar",
    "Add", "Subtract", "Divide", "Minimum", "Maximum", "Power", "Lerp", "Clamp", "Remap",
    "Smoothstep", "Saturate", "One Minus", "Abs", "Color Multiply", "Color Add",
    "Make Color", "Split Color", "Make Vector", "Split Vector", "Vector Scale", "Dot",
    "Normal Blend", "Noise", "Height Mask", "Slope Mask", "AO", "Curvature", "Cavity",
    "Material Layer", "Layer Blend", "Height Blend",
    "Normal Strength", "Normal Unpack", "Normal Flip Y", "Normalize", "Cross Product", "Length", "Distance",
    "Sine", "Cosine", "Fract", "Floor", "Ceil", "Step", "Sign", "Reflect",
    "Mask Combine", "Mask Levels", "Mask Contrast", "Mask Invert", "Fresnel",
    "Vector2 Parameter", "Vector3 Parameter"
};
static_assert(std::size(Names) == static_cast<size_t>(NodeKind::Count));
using Json = crude_json::value;
bool Number(const Json& object, const char* key, double& result)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) return false;
    result = object[key].get<crude_json::number>();
    return std::isfinite(result);
}
bool Id(const Json& object, const char* key, std::uint32_t& result, std::uint32_t limit = MaxId)
{
    double value;
    if (!Number(object, key, value) || value < 1 || value > limit || std::floor(value) != value) return false;
    result = static_cast<std::uint32_t>(value); return true;
}
template <std::size_t N> bool FloatArray(const Json& object, const char* key, std::array<float, N>& result)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) return false;
    const auto& a = object[key].get<crude_json::array>();
    if (a.size() != N) return false;
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!a[i].is_number()) return false;
        const double value = a[i].get<crude_json::number>();
        if (!std::isfinite(value) || std::abs(value) > 1000000) return false;
        result[i] = static_cast<float>(value);
    }
    return true;
}
template <std::size_t N> Json Array(const std::array<float, N>& source)
{
    Json result(crude_json::type_t::array);
    for (float value : source) result.push_back(Json(static_cast<double>(value)));
    return result;
}
}
const char* NodeKindName(NodeKind kind)
{
    const auto index = static_cast<size_t>(kind);
    return index < std::size(Names) ? Names[index] : "Unknown";
}
std::span<const NodeKind> AllNodeKinds()
{
    static const auto kinds = [] { std::array<NodeKind, static_cast<size_t>(NodeKind::Count)> result{};
        for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<NodeKind>(i); return result; }();
    return kinds;
}
const char* NodeCategory(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Scalar: case NodeKind::Color: case NodeKind::Vector2: case NodeKind::Vector3: return "Constants";
    case NodeKind::ScalarParameter: case NodeKind::ColorParameter: case NodeKind::TextureParameter: case NodeKind::Vector2Parameter: case NodeKind::Vector3Parameter: return "Parameters";
    case NodeKind::UV: case NodeKind::TilingOffset: case NodeKind::WorldPosition: case NodeKind::WorldNormal: case NodeKind::MakeVector: case NodeKind::SplitVector: case NodeKind::VectorScale: case NodeKind::Dot: return "Coordinates";
    case NodeKind::TextureSample: case NodeKind::Triplanar: return "Textures";
    case NodeKind::Noise: case NodeKind::HeightMask: case NodeKind::SlopeMask: case NodeKind::AO: case NodeKind::Curvature: case NodeKind::Cavity: return "Masks";
    case NodeKind::MaskCombine: case NodeKind::MaskLevels: case NodeKind::MaskContrast: case NodeKind::MaskInvert: case NodeKind::Fresnel: return "Masks";
    case NodeKind::NormalStrength: case NodeKind::NormalUnpack: case NodeKind::NormalFlipY: return "Materials";
    case NodeKind::MaterialLayer: case NodeKind::LayerBlend: case NodeKind::HeightBlend: case NodeKind::NormalBlend: case NodeKind::PbrOutput: return "Materials";
    case NodeKind::Blend: case NodeKind::ColorMultiply: case NodeKind::ColorAdd: case NodeKind::MakeColor: case NodeKind::SplitColor: return "Color";
    default: return "Math";
    }
}
const char* PinTypeName(PinType type)
{
    switch (type) {
    case P::Scalar: return "Scalar"; case P::Color: return "Color"; case P::Vector2: return "Vector2";
    case P::Vector3: return "Vector3"; case P::Texture: return "Texture"; case P::Surface: return "Surface";
    } return "Unknown";
}
std::span<const PinDefinition> NodePins(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Scalar: case NodeKind::ScalarParameter: case NodeKind::AO: case NodeKind::Curvature: case NodeKind::Cavity: return ScalarPins;
    case NodeKind::Color: case NodeKind::ColorParameter: return ColorPins;
    case NodeKind::Vector2: case NodeKind::Vector2Parameter: case NodeKind::UV: return V2Pins;
    case NodeKind::Vector3: case NodeKind::Vector3Parameter: case NodeKind::WorldPosition: case NodeKind::WorldNormal: return V3Pins;
    case NodeKind::TextureParameter: return TexturePins;
    case NodeKind::Multiply: case NodeKind::Add: case NodeKind::Subtract: case NodeKind::Divide: case NodeKind::Minimum: case NodeKind::Maximum: case NodeKind::Power: return MultiplyPins;
    case NodeKind::Lerp: return ThreeScalarPins;
    case NodeKind::Clamp: case NodeKind::Smoothstep: return ClampPins;
    case NodeKind::Remap: return RemapPins;
    case NodeKind::Saturate: case NodeKind::OneMinus: case NodeKind::Abs: return UnaryPins;
    case NodeKind::ColorMultiply: case NodeKind::ColorAdd: return ColorMathPins;
    case NodeKind::Blend: return BlendPins;
    case NodeKind::TilingOffset: return TilingPins;
    case NodeKind::TextureSample: return SamplePins;
    case NodeKind::Triplanar: return TriplanarPins;
    case NodeKind::MakeColor: return MakeColorPins;
    case NodeKind::SplitColor: return SplitColorPins;
    case NodeKind::MakeVector: return MakeVectorPins;
    case NodeKind::SplitVector: return SplitVectorPins;
    case NodeKind::VectorScale: return VectorScalePins;
    case NodeKind::Dot: return DotPins;
    case NodeKind::NormalBlend: return NormalBlendPins;
    case NodeKind::NormalStrength: return NormalStrengthPins;
    case NodeKind::NormalUnpack: return UnpackNormalPins;
    case NodeKind::NormalFlipY: case NodeKind::Normalize: return VectorUnaryPins;
    case NodeKind::Cross: case NodeKind::Reflect: return VectorBinaryPins;
    case NodeKind::Length: return VectorLengthPins;
    case NodeKind::Distance: return DotPins;
    case NodeKind::Sine: case NodeKind::Cosine: case NodeKind::Fract: case NodeKind::Floor: case NodeKind::Ceil: case NodeKind::Sign: case NodeKind::MaskInvert: return UnaryPins;
    case NodeKind::Step: case NodeKind::MaskCombine: return MultiplyPins;
    case NodeKind::MaskLevels: return LevelsPins;
    case NodeKind::MaskContrast: return ContrastPins;
    case NodeKind::Fresnel: return FresnelPins;
    case NodeKind::Noise: return NoisePins;
    case NodeKind::HeightMask: return HeightPins;
    case NodeKind::SlopeMask: return SlopePins;
    case NodeKind::MaterialLayer: return LayerPins;
    case NodeKind::LayerBlend: return LayerBlendPins;
    case NodeKind::HeightBlend: return HeightBlendPins;
    case NodeKind::PbrOutput: return OutputPins;
    default: return {};
    }
}
std::uint32_t PinId(std::uint32_t node, std::size_t slot) { return node * 16 + static_cast<std::uint32_t>(slot) + 1; }
GraphNode* MaterialGraphDocument::FindNode(std::uint32_t id)
{
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const auto& n) { return n.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}
const GraphNode* MaterialGraphDocument::FindNode(std::uint32_t id) const
{
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const auto& n) { return n.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}
std::optional<GraphPin> MaterialGraphDocument::FindPin(std::uint32_t id) const
{
    const auto* node = FindNode(id / 16);
    const auto slot = id % 16;
    if (!node || slot == 0 || slot > NodePins(node->kind).size()) return {};
    return GraphPin{node->id, NodePins(node->kind)[slot - 1]};
}
std::uint32_t MaterialGraphDocument::AddNode(NodeKind kind, std::array<float, 2> position)
{
    if (nodes.size() >= MaxNodes || nextId > MaxId || NodePins(kind).empty()) return 0;
    if (kind == NodeKind::PbrOutput && std::any_of(nodes.begin(), nodes.end(), [](const auto& n) { return n.kind == NodeKind::PbrOutput; })) return 0;
    const auto id = nextId++;
    GraphNode node{id, kind, NodeKindName(kind), {0.5f, 0.5f, 0.5f, 1.0f}, position};
    switch (kind) {
    case NodeKind::Vector2: case NodeKind::Vector3: node.value = {0, 0, 0, 0}; break;
    case NodeKind::TilingOffset: node.value = {1, 1, 0, 0}; break;
    case NodeKind::Noise: node.value = {1, 4, 0.5f, 0}; break;
    case NodeKind::HeightMask: node.value = {0, 10, 0, 0}; break;
    case NodeKind::SlopeMask: node.value = {20, 60, 0, 0}; break;
    case NodeKind::Triplanar: node.value = {1, 4, 0, 0}; break;
    case NodeKind::Clamp: case NodeKind::Smoothstep: node.value = {0.5f, 0, 1, 0}; break;
    case NodeKind::Remap: node.value = {0, 1, 0, 1}; node.settings[0] = 0.5f; break;
    case NodeKind::NormalBlend: node.value[0] = 1; break;
    case NodeKind::NormalStrength: node.value = {1,0,0,0}; break;
    case NodeKind::NormalUnpack: node.value = {.5f,.5f,1,1}; node.srgb = false; break;
    case NodeKind::NormalFlipY: case NodeKind::Normalize: case NodeKind::Length: node.value = {0,0,1,0}; break;
    case NodeKind::MaskLevels: node.value = {.5f,0,1,1}; break;
    case NodeKind::MaskContrast: node.value = {.5f,1,0,0}; break;
    case NodeKind::Fresnel: node.value = {5,0,0,0}; break;
    case NodeKind::Blend: node.settings[0] = 0.5f; break;
    case NodeKind::VectorScale: node.settings[0] = 1; break;
    case NodeKind::HeightBlend: node.value = {0.5f, 0.2f, 0, 0}; break;
    case NodeKind::MaterialLayer: case NodeKind::PbrOutput: node.value = {0.5f, 0.5f, 0.5f, 1}; node.settings = {0.5f, 0, 1, 0}; break;
    case NodeKind::TextureSample: case NodeKind::TextureParameter: node.value = {1, 1, 1, 1}; break;
    default: break;
    }
    if (kind == NodeKind::ScalarParameter || kind == NodeKind::ColorParameter || kind == NodeKind::TextureParameter || kind == NodeKind::Vector2Parameter || kind == NodeKind::Vector3Parameter) {
        node.parameter = "Parameter_" + std::to_string(id); node.settings = {0, 1, 0, 0};
    }
    nodes.push_back(std::move(node));
    return id;
}
bool MaterialGraphDocument::RemoveLink(std::uint32_t id, bool managedEdit)
{
    if (!managedEdit) for (const auto& link : links) if (link.id == id) {
        const auto* target = FindNode(link.input / 16);
        if (target && target->region != GraphRegion::Custom) return false;
    }
    return std::erase_if(links, [id](const auto& link) { return link.id == id; }) > 0;
}
bool MaterialGraphDocument::RemoveNode(std::uint32_t id, bool managedEdit)
{
    const auto* node = FindNode(id);
    if (!managedEdit && node && (node->region != GraphRegion::Custom ||
        std::any_of(links.begin(), links.end(), [&](const auto& l) { return l.output / 16 == id && FindNode(l.input / 16)->region != GraphRegion::Custom; }))) return false;
    if (!std::erase_if(nodes, [id](const auto& node) { return node.id == id; })) return false;
    std::erase_if(links, [id](const auto& link) { return link.output / 16 == id || link.input / 16 == id; });
    return true;
}
bool MaterialGraphDocument::CanConnect(std::uint32_t first, std::uint32_t second, std::string& reason, bool managedEdit) const
{
    reason.clear();
    const auto a = FindPin(first), b = FindPin(second);
    if (!a || !b) { reason = "Choose two existing pins"; return false; }
    if (a->definition.input == b->definition.input) { reason = "Connect an output to an input"; return false; }
    if (a->definition.type != b->definition.type) { reason = "Pin types must match: " + std::string(PinTypeName(a->definition.type)) + " / " + PinTypeName(b->definition.type); return false; }
    const auto source = a->definition.input ? b->node : a->node;
    const auto target = a->definition.input ? a->node : b->node;
    if (!managedEdit && FindNode(target)->region != GraphRegion::Custom) { reason = "Managed layer connections are read-only; use Layer Stack or Convert to Custom Graph"; return false; }
    if (source == target) { reason = "A node cannot connect to itself"; return false; }
    // Adding source -> target is cyclic if target already reaches source.
    std::unordered_map<std::uint32_t,std::vector<std::uint32_t>> outgoing;
    for(const auto& link:links)outgoing[link.output/16].push_back(link.input/16);
    std::vector<std::uint32_t> pending{target};
    std::unordered_set<std::uint32_t> visited;
    while (!pending.empty())
    {
        const auto current = pending.back(); pending.pop_back();
        if (current == source) { reason = "This connection would create a cycle"; return false; }
        if (!visited.insert(current).second) continue;
        for (const auto next : outgoing[current]) pending.push_back(next);
    }
    return true;
}
bool MaterialGraphDocument::Connect(std::uint32_t first, std::uint32_t second, std::string& reason, bool managedEdit)
{
    if (!CanConnect(first, second, reason, managedEdit)) return false;
    if (FindPin(first)->definition.input) std::swap(first, second);
    if (std::any_of(links.begin(), links.end(), [&](const auto& l) { return l.output == first && l.input == second; })) return false;
    if (links.size() >= MaxLinks || nextId > MaxId) { reason = "Graph size limit reached"; return false; }
    // An input has one source. Rewiring replaces that source only after validation.
    std::erase_if(links, [second](const auto& link) { return link.input == second; });
    links.push_back({nextId++, first, second});
    return true;
}
MaterialGraphDocument MaterialGraphDocument::Example()
{
    MaterialGraphDocument d;
    const auto color = d.AddNode(NodeKind::Color, {20, 40});
    const auto roughness = d.AddNode(NodeKind::Scalar, {20, 240});
    const auto output = d.AddNode(NodeKind::PbrOutput, {360, 90});
    d.FindNode(color)->name = "Base Color";
    d.FindNode(color)->value = {0.22f, 0.38f, 0.19f, 1.0f};
    d.FindNode(roughness)->name = "Roughness";
    d.FindNode(roughness)->value[0] = 0.7f;
    std::string reason;
    d.Connect(PinId(color, 0), PinId(output, 0), reason);
    d.Connect(PinId(roughness, 0), PinId(output, 1), reason);
    return d;
}
MaterialGraphDocument MaterialGraphDocument::WeatheredMetal()
{
    MaterialGraphDocument d;std::string reason;
    const auto iron=d.AddNode(NodeKind::MaterialLayer,{300,50}),rust=d.AddNode(NodeKind::MaterialLayer,{300,460});
    d.FindNode(iron)->name="Iron";d.FindNode(iron)->value={.08f,.11f,.13f,1};d.FindNode(iron)->settings={.26f,1,1,.5f};
    d.FindNode(rust)->name="Rust";d.FindNode(rust)->value={.4f,.13f,.025f,1};d.FindNode(rust)->settings={.88f,0,1,.2f};
    const auto noise=d.AddNode(NodeKind::Noise,{-300,40});d.FindNode(noise)->value={9,4,.55f,7};
    const auto remap=d.AddNode(NodeKind::Remap,{-10,40});d.FindNode(remap)->value={.35f,.7f,0,1};d.Connect(PinId(noise,2),PinId(remap,0),reason);
    const auto saturate=d.AddNode(NodeKind::Saturate,{-10,420});d.Connect(PinId(remap,5),PinId(saturate,0),reason);
    const auto amount=d.AddNode(NodeKind::ScalarParameter,{-300,500});d.FindNode(amount)->name="Rust amount";d.FindNode(amount)->parameter="RustAmount";d.FindNode(amount)->value[0]=.8f;
    const auto multiply=d.AddNode(NodeKind::Multiply,{300,880});d.Connect(PinId(saturate,1),PinId(multiply,0),reason);d.Connect(PinId(amount,0),PinId(multiply,1),reason);
    const auto blend=d.AddNode(NodeKind::LayerBlend,{610,280});d.Connect(PinId(iron,8),PinId(blend,0),reason);d.Connect(PinId(rust,8),PinId(blend,1),reason);d.Connect(PinId(multiply,2),PinId(blend,2),reason);
    const auto output=d.AddNode(NodeKind::PbrOutput,{930,280});d.Connect(PinId(blend,3),PinId(output,8),reason);return d;
}
MaterialGraphDocument MaterialGraphDocument::MossyRock()
{
    MaterialGraphDocument d;std::string reason;
    const auto rock=d.AddNode(NodeKind::MaterialLayer,{600,50}),moss=d.AddNode(NodeKind::MaterialLayer,{600,440});
    d.FindNode(rock)->name="Rock";d.FindNode(rock)->value={.19f,.16f,.13f,1};d.FindNode(rock)->settings={.78f,0,1,.45f};
    d.FindNode(moss)->name="Moss";d.FindNode(moss)->value={.06f,.18f,.025f,1};d.FindNode(moss)->settings={.95f,0,1,.55f};
    const auto slope=d.AddNode(NodeKind::SlopeMask,{-300,40});d.FindNode(slope)->value={25,75,0,0};
    const auto invert=d.AddNode(NodeKind::OneMinus,{0,40});d.Connect(PinId(slope,3),PinId(invert,0),reason);
    const auto noise=d.AddNode(NodeKind::Noise,{-300,360});d.FindNode(noise)->value={.5f,4,.55f,23};d.FindNode(noise)->settings[0]=1;
    const auto mask=d.AddNode(NodeKind::Multiply,{300,170});d.Connect(PinId(invert,1),PinId(mask,0),reason);d.Connect(PinId(noise,2),PinId(mask,1),reason);
    const auto amount=d.AddNode(NodeKind::ScalarParameter,{-300,660});d.FindNode(amount)->parameter="MossAmount";d.FindNode(amount)->name="Moss amount";d.FindNode(amount)->value[0]=1.5f;
    const auto coverage=d.AddNode(NodeKind::Multiply,{300,500});d.Connect(PinId(mask,2),PinId(coverage,0),reason);d.Connect(PinId(amount,0),PinId(coverage,1),reason);
    const auto blend=d.AddNode(NodeKind::HeightBlend,{920,280});d.FindNode(blend)->value[1]=.4f;d.Connect(PinId(rock,8),PinId(blend,0),reason);d.Connect(PinId(moss,8),PinId(blend,1),reason);d.Connect(PinId(coverage,2),PinId(blend,2),reason);
    const auto output=d.AddNode(NodeKind::PbrOutput,{1240,280});d.Connect(PinId(blend,4),PinId(output,8),reason);return d;
}
std::string MaterialGraphDocument::ToJson() const
{
    Json root(crude_json::type_t::object);
    const bool managed = !layers.empty() || std::any_of(nodes.begin(), nodes.end(), [](const auto& n) {
        return n.region != GraphRegion::Custom || n.owner != 0 || n.kind >= NodeKind::NormalStrength ||
            ((n.kind == NodeKind::LayerBlend || n.kind == NodeKind::HeightBlend) && n.settings[3] == 1);
    }) || std::any_of(links.begin(), links.end(), [&](const auto& l) {
        const auto* n = FindNode(l.input / 16); return n && n->kind == NodeKind::Noise && l.input == PinId(n->id,3);
    });
    root["format"] = "ixtreeme.materialgraph"; root["version"] = managed ? 3.0 : 2.0;
    root["next_id"] = static_cast<double>(nextId);
    root["nodes"] = Json(crude_json::type_t::array); root["links"] = Json(crude_json::type_t::array);
    for (const auto& node : nodes)
    {
        Json n(crude_json::type_t::object);
        n["id"] = static_cast<double>(node.id); n["kind"] = NodeKindName(node.kind); n["name"] = node.name;
        n["value"] = Array(node.value); n["position"] = Array(node.position); n["settings"] = Array(node.settings);
        n["resource"] = node.resource; n["asset_guid"] = node.assetGuid; n["asset_id"] = node.assetId; n["parameter"] = node.parameter; n["srgb"] = node.srgb;
        if (managed) { n["region"] = static_cast<double>(node.region); n["owner"] = static_cast<double>(node.owner); }
        root["nodes"].push_back(std::move(n));
    }
    for (const auto& link : links)
    {
        Json l(crude_json::type_t::object);
        l["id"] = static_cast<double>(link.id); l["output"] = static_cast<double>(link.output); l["input"] = static_cast<double>(link.input);
        root["links"].push_back(std::move(l));
    }
    if (managed) {
        root["layers"] = Json(crude_json::type_t::array);
        for (const auto& layer : layers) {
            Json l(crude_json::type_t::object);
            l["id"] = static_cast<double>(layer.id); l["source"] = static_cast<double>(layer.sourceNode);
            l["opacity"] = static_cast<double>(layer.opacityNode); l["weight"] = static_cast<double>(layer.weightNode);
            l["blend"] = static_cast<double>(layer.blendNode); l["mask"] = static_cast<double>(layer.maskPin); l["enabled"] = layer.enabled;
            root["layers"].push_back(std::move(l));
        }
    }
    return root.dump(2);
}
bool MaterialGraphDocument::FromJson(const std::string& text, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (text.size() > 4 * 1024 * 1024) return fail("Graph file is too large");
    const Json root = Json::parse(text);
    double version;
    if (!root.is_object() || !root.contains("format") || !root["format"].is_string() || root["format"].get<crude_json::string>() != "ixtreeme.materialgraph") return fail("Not a Material Graph file");
    if (!Number(root, "version", version) || (version != 1 && version != 2 && version != 3)) return fail("Unsupported Material Graph version");
    if (!root.contains("nodes") || !root["nodes"].is_array() || !root.contains("links") || !root["links"].is_array()) return fail("Missing graph nodes or links");
    const auto& sourceNodes = root["nodes"].get<crude_json::array>();
    const auto& sourceLinks = root["links"].get<crude_json::array>();
    if (sourceNodes.size() > MaxNodes || sourceLinks.size() > MaxLinks) return fail("Graph size limit exceeded");
    MaterialGraphDocument draft;
    std::unordered_set<std::uint32_t> ids;
    int outputs = 0;
    for (const auto& source : sourceNodes)
    {
        GraphNode node;
        if (!Id(source, "id", node.id) || !ids.insert(node.id).second || !FloatArray(source, "position", node.position) || !FloatArray(source, "value", node.value)) return fail("Invalid or duplicate node data");
        if (!source.contains("kind") || !source["kind"].is_string() || !source.contains("name") || !source["name"].is_string()) return fail("Missing node kind or name");
        bool found = false;
        for (auto kind : AllNodeKinds())
            if (source["kind"].get<crude_json::string>() == NodeKindName(kind)) { node.kind = kind; found = true; break; }
        if (!found) return fail("Unknown node kind");
        if (node.kind == NodeKind::PbrOutput && ++outputs > 1) return fail("Only one PBR Output is allowed");
        node.name = source["name"].get<crude_json::string>();
        if (node.name.size() > 255) return fail("Node name is too long");
        if (version >= 2) {
            if (!FloatArray(source, "settings", node.settings)) return fail("Invalid node settings");
            for (const auto* key : {"resource", "asset_guid", "parameter"})
                if (!source.contains(key) || !source[key].is_string() || source[key].get<crude_json::string>().size() > 4096) return fail("Invalid resource or parameter");
            node.resource = source["resource"].get<crude_json::string>(); node.assetGuid = source["asset_guid"].get<crude_json::string>(); node.parameter = source["parameter"].get<crude_json::string>();
            if (!source.contains("srgb") || !source["srgb"].is_boolean()) return fail("Invalid color space");
            node.srgb = source["srgb"].get<crude_json::boolean>();
            if(source.contains("asset_id")) {if(!source["asset_id"].is_string() || source["asset_id"].get<crude_json::string>().size()>255)return fail("Invalid asset ID");node.assetId=source["asset_id"].get<crude_json::string>();}
            if(node.parameter.size()>255)return fail("Parameter name is too long");
            if (version == 3) {
                double region, owner;
                if (!Number(source, "region", region) || region < 0 || region > 2 || std::floor(region) != region ||
                    !Number(source, "owner", owner) || owner < 0 || owner > MaxId || std::floor(owner) != owner) return fail("Invalid graph ownership");
                node.region = static_cast<GraphRegion>(static_cast<int>(region)); node.owner = static_cast<std::uint32_t>(owner);
            }
        } else if (node.kind == NodeKind::PbrOutput) node.settings = {0.5f, 0, 1, 0};
        draft.nextId = std::max(draft.nextId, node.id + 1);
        draft.nodes.push_back(std::move(node));
    }
    if (version == 3) {
        if (!root.contains("layers") || !root["layers"].is_array() ||
            root["layers"].get<crude_json::array>().size() > 256) return fail("Invalid material layers");
        for (const auto& source : root["layers"].get<crude_json::array>()) {
            MaterialLayer layer; double mask = 0;
            if (!Id(source, "id", layer.id) || !ids.insert(layer.id).second || !Id(source, "source", layer.sourceNode) ||
                !Id(source, "opacity", layer.opacityNode) || !Id(source, "weight", layer.weightNode) || !Id(source, "blend", layer.blendNode) ||
                !Number(source, "mask", mask) || mask < 0 || mask > MaxId * 16 + 14 || std::floor(mask) != mask ||
                !source.contains("enabled") || !source["enabled"].is_boolean()) return fail("Invalid layer IDs or settings");
            layer.maskPin = static_cast<std::uint32_t>(mask); layer.enabled = source["enabled"].get<crude_json::boolean>();
            draft.nextId = std::max(draft.nextId, layer.id + 1); draft.layers.push_back(layer);
        }
    }
    std::unordered_set<std::uint32_t> inputs;
    for (const auto& source : sourceLinks)
    {
        GraphLink link;
        if (!Id(source, "id", link.id) || !ids.insert(link.id).second || !Id(source, "output", link.output, MaxId * 16 + 14) || !Id(source, "input", link.input, MaxId * 16 + 14)) return fail("Invalid or duplicate link data");
        const auto from = draft.FindPin(link.output), to = draft.FindPin(link.input);
        if (!from || !to || from->definition.input || !to->definition.input || from->definition.type!=to->definition.type || from->node==to->node || !inputs.insert(link.input).second) return fail("Invalid, conflicting or cyclic connection");
        draft.nextId = std::max(draft.nextId, link.id + 1);
        draft.links.push_back(link);
    }
    // Validate all edges in one pass, independent of their saved order.
    std::unordered_map<std::uint32_t,int> degree;
    std::unordered_map<std::uint32_t,std::vector<std::uint32_t>> outgoing;
    for(const auto& node:draft.nodes)degree[node.id]=0;
    for(const auto& link:draft.links){++degree[link.input/16];outgoing[link.output/16].push_back(link.input/16);}
    std::vector<std::uint32_t> ready;for(const auto& node:draft.nodes)if(degree[node.id]==0)ready.push_back(node.id);
    for(size_t head=0;head<ready.size();++head)for(const auto next:outgoing[ready[head]])if(--degree[next]==0)ready.push_back(next);
    if(ready.size()!=draft.nodes.size())return fail("Cyclic connection");
    if (root.contains("next_id"))
    {
        std::uint32_t next;
        if (!Id(root, "next_id", next, MaxId + 1) || next < draft.nextId) return fail("Invalid next graph ID");
        draft.nextId = next;
    }
    if (!draft.ValidateLayerStack(error)) return false;
    *this = std::move(draft);
    return true;
}
}
