#include "MaterialGraphDocument.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_set>

namespace material_tool
{
namespace
{
constexpr std::uint32_t MaxId = 0x00ffffff;
using Connections = std::map<std::uint32_t, std::uint32_t>; // input -> output, 0 removes input
std::uint32_t BlendPin(const MaterialGraphDocument& d, const MaterialLayer& layer)
{ return PinId(layer.blendNode, d.FindNode(layer.blendNode)->kind == NodeKind::HeightBlend ? 4 : 3); }
Connections LayerConnections(const MaterialGraphDocument& d)
{
    Connections result;
    std::uint32_t previous = 0;
    for (const auto& layer : d.layers)
    {
        result[PinId(layer.weightNode, 0)] = layer.maskPin;
        result[PinId(layer.weightNode, 1)] = PinId(layer.opacityNode, 0);
        result[PinId(layer.blendNode, 0)] = layer.enabled ? previous : 0;
        result[PinId(layer.blendNode, 1)] = PinId(layer.sourceNode, 8);
        result[PinId(layer.blendNode, 2)] = PinId(layer.weightNode, 2);
        if (layer.enabled) previous = BlendPin(d, layer);
    }
    for (const auto& node : d.nodes) if (node.region == GraphRegion::Output) result[PinId(node.id, 8)] = previous;
    return result;
}
bool CheckAndCommit(MaterialGraphDocument& target, MaterialGraphDocument& draft, std::string& error)
{
    MaterialGraphDocument checked;
    if (!checked.FromJson(draft.ToJson(), error)) return false;
    target = std::move(draft); return true;
}
}
MaterialLayer* MaterialGraphDocument::FindLayer(std::uint32_t id)
{
    const auto found = std::find_if(layers.begin(), layers.end(), [id](const auto& layer) { return layer.id == id; });
    return found == layers.end() ? nullptr : &*found;
}
const MaterialLayer* MaterialGraphDocument::FindLayer(std::uint32_t id) const
{
    const auto found = std::find_if(layers.begin(), layers.end(), [id](const auto& layer) { return layer.id == id; });
    return found == layers.end() ? nullptr : &*found;
}
bool MaterialGraphDocument::HasLayerStack() const
{
    return !layers.empty() || std::any_of(nodes.begin(), nodes.end(), [](const auto& node) { return node.region == GraphRegion::Output; });
}
bool MaterialGraphDocument::ValidateLayerStack(std::string& error) const
{
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    std::unordered_set<std::uint32_t> owners, ownedNodes;
    for (const auto& layer : layers)
    {
        if (!layer.id || !owners.insert(layer.id).second) return fail("Invalid or duplicate material layer ID");
        for (const auto id : {layer.sourceNode, layer.opacityNode, layer.weightNode, layer.blendNode})
        {
            const auto* node = FindNode(id);
            if (!node || node->region != GraphRegion::ManagedLayer || node->owner != layer.id || !ownedNodes.insert(id).second)
                return fail("Layer refers to missing or conflicting owned nodes");
        }
        if (FindNode(layer.sourceNode)->kind != NodeKind::MaterialLayer || FindNode(layer.opacityNode)->kind != NodeKind::ScalarParameter ||
            FindNode(layer.weightNode)->kind != NodeKind::Multiply ||
            (FindNode(layer.blendNode)->kind != NodeKind::LayerBlend && FindNode(layer.blendNode)->kind != NodeKind::HeightBlend))
            return fail("Invalid managed layer node types");
        if (FindNode(layer.weightNode)->value[0] != 1) return fail("Unmasked layer must have unit mask weight");
        const float channels = FindNode(layer.blendNode)->settings[2];
        if (!std::isfinite(channels) || channels < 0 || channels > 255 || std::floor(channels) != channels || FindNode(layer.blendNode)->settings[3] != 1) return fail("Invalid layer channel mask");
        if (layer.maskPin)
        {
            const auto mask = FindPin(layer.maskPin);
            if (!mask || mask->definition.input || mask->definition.type != PinType::Scalar) return fail("Missing or invalid layer mask output");
        }
    }
    int outputs = 0;
    for (const auto& node : nodes)
    {
        if (node.region == GraphRegion::Custom && node.owner) return fail("Custom node has a managed owner");
        if (node.region == GraphRegion::ManagedLayer && (!owners.contains(node.owner) || !ownedNodes.contains(node.id))) return fail("Orphan managed layer node");
        if (node.region == GraphRegion::Output)
        {
            if (node.kind != NodeKind::PbrOutput || node.owner) return fail("Invalid managed output");
            ++outputs;
            for (const auto& link : links) if (link.input / 16 == node.id && link.input != PinId(node.id, 8))
                return fail("Managed output channels cannot override the layer stack");
        }
    }
    if (layers.empty() && outputs == 0) return true;
    if (outputs != 1) return fail("Layer stack needs exactly one managed output");
    const auto desired = LayerConnections(*this);
    for (const auto& [input, output] : desired)
    {
        const auto found = std::find_if(links.begin(), links.end(), [&](const auto& l) { return l.input == input; });
        if ((output && (found == links.end() || found->output != output)) || (!output && found != links.end()))
            return fail("Managed graph topology disagrees with Layer Stack; convert explicitly to a custom graph");
    }
    return true;
}
bool MaterialGraphDocument::RebuildLayerLinks(std::string& error)
{
    error.clear();
    const auto desired = LayerConnections(*this);
    for (const auto& [input, output] : desired)
    {
        const auto found = std::find_if(links.begin(), links.end(), [&](const auto& l) { return l.input == input; });
        if (!output) { std::erase_if(links, [&](const auto& l) { return l.input == input; }); continue; }
        if (found != links.end()) found->output = output;
        else
        {
            if (nextId > MaxId || links.size() >= 8192) { error = "Graph ID or link limit exceeded"; return false; }
            links.push_back({nextId++, output, input});
        }
    }
    return true;
}
std::uint32_t MaterialGraphDocument::AddMaterialLayer(const std::string& name, std::string& error)
{
    error.clear();
    if (name.empty() || name.size() > 255 || layers.size() >= 256 || nodes.size() > 2043 || nextId > MaxId - 16)
    { error = "Invalid layer name or graph size limit"; return 0; }
    if (layers.empty() && std::any_of(nodes.begin(), nodes.end(), [](const auto& n) { return n.kind == NodeKind::PbrOutput && n.region != GraphRegion::Output; }))
    { error = "Create a layer preset to start a stack; an existing custom output is preserved"; return 0; }
    auto draft = *this;
    if (!draft.HasLayerStack())
    {
        const auto output = draft.AddNode(NodeKind::PbrOutput, {1500, 200});
        draft.FindNode(output)->region = GraphRegion::Output;
    }
    MaterialLayer layer; layer.id = draft.nextId++;
    const float row = static_cast<float>(draft.layers.size()) * 500;
    layer.sourceNode = draft.AddNode(NodeKind::MaterialLayer, {500, row});
    layer.opacityNode = draft.AddNode(NodeKind::ScalarParameter, {0, row + 240});
    layer.weightNode = draft.AddNode(NodeKind::Multiply, {300, row + 240});
    layer.blendNode = draft.AddNode(NodeKind::LayerBlend, {1100, row});
    for (const auto id : {layer.sourceNode, layer.opacityNode, layer.weightNode, layer.blendNode})
    { auto& node = *draft.FindNode(id); node.region = GraphRegion::ManagedLayer; node.owner = layer.id; }
    draft.FindNode(layer.sourceNode)->name = name;
    auto& opacity = *draft.FindNode(layer.opacityNode);
    opacity.name = name + " opacity"; opacity.parameter = "LayerOpacity_" + std::to_string(layer.id); opacity.value[0] = 1;
    draft.FindNode(layer.weightNode)->value[0] = 1;
    draft.FindNode(layer.blendNode)->settings[2] = 255;
    draft.FindNode(layer.blendNode)->settings[3] = 1;
    draft.layers.push_back(layer);
    if (!draft.RebuildLayerLinks(error) || !CheckAndCommit(*this, draft, error)) return 0;
    return layer.id;
}
bool MaterialGraphDocument::MoveMaterialLayer(std::uint32_t id, std::size_t index, std::string& error)
{
    error.clear();
    if (!FindLayer(id) || index >= layers.size()) { error = "Missing layer or invalid layer index"; return false; }
    auto draft = *this; const auto layer = *draft.FindLayer(id);
    std::erase_if(draft.layers, [id](const auto& l) { return l.id == id; });
    draft.layers.insert(draft.layers.begin() + static_cast<std::ptrdiff_t>(index), layer);
    return draft.RebuildLayerLinks(error) && CheckAndCommit(*this, draft, error);
}
bool MaterialGraphDocument::SetMaterialLayerEnabled(std::uint32_t id, bool enabled, std::string& error)
{
    error.clear();
    if (!FindLayer(id)) { error = "Missing material layer"; return false; }
    auto draft = *this; draft.FindLayer(id)->enabled = enabled;
    return draft.RebuildLayerLinks(error) && CheckAndCommit(*this, draft, error);
}
bool MaterialGraphDocument::SetMaterialLayerMask(std::uint32_t id, std::uint32_t maskPin, std::string& error)
{
    error.clear();
    const auto mask = FindPin(maskPin);
    if (!FindLayer(id) || (maskPin && (!mask || mask->definition.input || mask->definition.type != PinType::Scalar)))
    { error = "Missing layer or invalid mask output"; return false; }
    auto draft = *this; draft.FindLayer(id)->maskPin = maskPin;
    return draft.RebuildLayerLinks(error) && CheckAndCommit(*this, draft, error);
}
bool MaterialGraphDocument::SetMaterialLayerHeightBlend(std::uint32_t id, bool heightBlend, std::string& error)
{
    error.clear();
    if (!FindLayer(id)) { error = "Missing material layer"; return false; }
    auto draft = *this; auto& layer = *draft.FindLayer(id); const auto previous = BlendPin(draft, layer);
    auto& node = *draft.FindNode(layer.blendNode); node.kind = heightBlend ? NodeKind::HeightBlend : NodeKind::LayerBlend;
    node.value[1] = std::max(node.value[1], 0.2f);
    for (auto& link : draft.links) if (link.output == previous) link.output = BlendPin(draft, layer);
    if (!heightBlend) std::erase_if(draft.links, [&](const auto& l) { return l.input == PinId(layer.blendNode, 3); });
    return draft.RebuildLayerLinks(error) && CheckAndCommit(*this, draft, error);
}
bool MaterialGraphDocument::RemoveMaterialLayer(std::uint32_t id, std::string& error)
{
    error.clear();
    if (!FindLayer(id)) { error = "Missing material layer"; return false; }
    for (const auto& link : links)
    {
        const auto* from = FindNode(link.output / 16); const auto* to = FindNode(link.input / 16);
        if (from->owner == id && to->region == GraphRegion::Custom) { error = "Custom nodes reference this layer; detach or remove those connections first"; return false; }
    }
    for (const auto& layer : layers) if (layer.id != id && layer.maskPin && FindNode(layer.maskPin / 16)->owner == id)
    { error = "Another layer uses this layer's mask output"; return false; }
    auto draft = *this;
    std::unordered_set<std::uint32_t> removed;
    for (const auto& node : draft.nodes) if (node.owner == id) removed.insert(node.id);
    std::erase_if(draft.links, [&](const auto& l) { return removed.contains(l.output / 16) || removed.contains(l.input / 16); });
    std::erase_if(draft.nodes, [&](const auto& n) { return removed.contains(n.id); });
    std::erase_if(draft.layers, [id](const auto& l) { return l.id == id; });
    return draft.RebuildLayerLinks(error) && CheckAndCommit(*this, draft, error);
}
std::uint32_t MaterialGraphDocument::DuplicateMaterialLayer(std::uint32_t id, std::string& error)
{
    error.clear();
    const auto* original = FindLayer(id);
    if (!original) { error = "Missing material layer"; return 0; }
    auto draft = *this;
    const auto copyId = draft.AddMaterialLayer(FindNode(original->sourceNode)->name + " copy", error);
    if (!copyId) return 0;
    auto& copy = *draft.FindLayer(copyId); copy.maskPin = original->maskPin; copy.enabled = original->enabled;
    for (const auto pair : {std::pair{original->sourceNode, copy.sourceNode}, std::pair{original->opacityNode, copy.opacityNode}, std::pair{original->blendNode, copy.blendNode}})
    {
        const auto& from = *FindNode(pair.first); auto& to = *draft.FindNode(pair.second);
        to.value = from.value; to.settings = from.settings; to.kind = from.kind;
    }
    for (const auto& link : links) if (link.input / 16 == original->sourceNode)
        draft.links.push_back({draft.nextId++, link.output, PinId(copy.sourceNode, link.input % 16 - 1)});
    if (!draft.RebuildLayerLinks(error) || !CheckAndCommit(*this, draft, error)) return 0;
    return copyId;
}
void MaterialGraphDocument::ConvertToCustomGraph()
{
    for (auto& node : nodes) { node.region = GraphRegion::Custom; node.owner = 0; }
    layers.clear();
}
}
