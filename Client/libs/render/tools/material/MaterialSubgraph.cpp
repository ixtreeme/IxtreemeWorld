#include "MaterialSubgraph.h"
#include "MaterialInstance.h"
#include <crude_json.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace material_tool
{
namespace
{
using Json = crude_json::value;
constexpr std::uint32_t MaxId = 0x00ffffff;
bool IsParameter(NodeKind kind)
{
    return kind == NodeKind::ScalarParameter || kind == NodeKind::ColorParameter || kind == NodeKind::TextureParameter ||
        kind == NodeKind::Vector2Parameter || kind == NodeKind::Vector3Parameter;
}
std::optional<NodeKind> ConstantKind(PinType type)
{
    switch (type) {
    case PinType::Scalar: return NodeKind::Scalar;
    case PinType::Color: return NodeKind::Color;
    case PinType::Vector2: return NodeKind::Vector2;
    case PinType::Vector3: return NodeKind::Vector3;
    default: return {};
    }
}
std::uint32_t InputSource(const MaterialGraphDocument& graph, std::uint32_t pin)
{
    for (const auto& link : graph.links) if (link.input == pin) return link.output;
    return 0;
}
using Origins = std::map<std::uint32_t, MaterialNodeOrigin>;
class Expander
{
public:
    explicit Expander(const MaterialSubgraphResolver& resolver) : resolver_(resolver) {}
    bool Calls(MaterialGraphDocument& graph, Origins& origins, std::string& error, std::uint32_t& preview)
    {
        std::vector<GraphNode> calls;
        for (const auto& node : graph.nodes) if (node.kind == NodeKind::Subgraph) calls.push_back(node);
        std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (const auto& call : calls) {
            if (++calls_ > 256) { error = "Subgraph expansion exceeds 256 calls"; return false; }
            if (!IsMaterialAssetGuid(call.assetGuid)) { error = "Subgraph node " + std::to_string(call.id) + ": missing or invalid asset GUID"; return false; }
            if (std::find(stack_.begin(), stack_.end(), call.assetGuid) != stack_.end()) {
                error = "Recursive subgraph dependency";
                for (const auto& guid : stack_) error += " -> " + guid;
                error += " -> " + call.assetGuid; return false;
            }
            if (stack_.size() >= 16) { error = "Subgraph dependency depth exceeds 16"; return false; }
            auto found = assets_.find(call.assetGuid);
            if (found == assets_.end()) {
                MaterialSubgraph asset;
                if (!resolver_ || !resolver_(call.assetGuid, asset, error)) {
                    if (error.empty()) error = "Missing subgraph asset: " + call.assetGuid;
                    error = "Subgraph node " + std::to_string(call.id) + ": " + error; return false;
                }
                // Resolver results are untrusted data, just like files on disk.
                MaterialSubgraph checked;
                if (!checked.FromJson(asset.ToJson(), error)) return false;
                std::vector<MaterialGraphPort> ports;
                if (!checked.GetInterface(ports,error)) return false;
                found = assets_.emplace(call.assetGuid, ResolvedAsset{std::move(checked),std::move(ports)}).first;
            }
            const auto& asset = found->second.asset; // unordered_map rehash preserves element references
            const auto& ports = found->second.ports;
            if (asset.interfaceVersion != call.interfaceVersion || ports.size() != call.ports.size()) {
                error = "Subgraph node " + std::to_string(call.id) + ": interface version/signature changed; refresh explicitly"; return false;
            }
            for (std::size_t at = 0; at < ports.size(); ++at) {
                const auto& current = ports[at]; const auto& saved = call.ports[at];
                if (current.id != saved.id || current.name != saved.name || current.type != saved.type || current.input != saved.input) {
                    error = "Subgraph node " + std::to_string(call.id) + ": port signature changed; refresh explicitly"; return false;
                }
            }
            dependencies.insert(call.assetGuid);
            auto body = asset.graph; Origins bodyOrigins; std::uint32_t unusedPreview = 0;
            stack_.push_back(call.assetGuid);
            const bool success = Calls(body, bodyOrigins, error, unusedPreview);
            stack_.pop_back();
            if (!success) { error = "In subgraph " + call.assetGuid + " (node " + std::to_string(call.id) + "): " + error; return false; }
            if (!Inline(graph, origins, call, body, bodyOrigins, error, preview)) return false;
        }
        return true;
    }
    std::unordered_set<std::string> dependencies;
private:
    bool Allocate(MaterialGraphDocument& graph, std::uint32_t& id, std::string& error)
    {
        if (graph.nextId > MaxId) { error = "Subgraph expansion exhausts graph IDs"; return false; }
        id = graph.nextId++; return true;
    }
    bool Inline(MaterialGraphDocument& graph, Origins& origins, const GraphNode& call,
                const MaterialGraphDocument& body, const Origins& bodyOrigins, std::string& error, std::uint32_t& preview)
    {
        std::unordered_map<std::uint32_t, std::uint32_t> inputs, nodes, outputs;
        for (std::size_t at = 0; at < call.ports.size(); ++at) if (call.ports[at].input)
            inputs[call.ports[at].id] = InputSource(graph, PinId(call.id, at));
        std::erase_if(graph.nodes, [&](const auto& n) { return n.id == call.id; });
        std::erase_if(graph.links, [&](const auto& l) { return l.input / 16 == call.id; });
        auto sorted = body.nodes;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto prefix = "Subgraph_" + std::to_string(call.id) + "/";
        for (auto node : sorted) {
            if (node.kind == NodeKind::SubgraphOutput) continue;
            const auto localId = node.id;
            if (node.kind == NodeKind::SubgraphInput) {
                if (inputs.at(localId)) continue;
                const auto kind = ConstantKind(node.interfaceType);
                if (!kind) { error = "Subgraph node " + std::to_string(call.id) + ": required Texture/Surface input " + node.parameter + " is unbound"; return false; }
                node.kind = *kind; node.parameter.clear();
                for (const auto& port : call.ports) if (port.id == localId) node.value = port.defaultValue;
            }
            if (graph.nodes.size() >= 2048 || !Allocate(graph, node.id, error)) {
                if (error.empty()) error = "Expanded graph exceeds 2048 nodes"; return false;
            }
            node.region = GraphRegion::Custom; node.owner = 0;
            if (IsParameter(node.kind)) {
                node.parameter = prefix + node.parameter;
                if (node.parameter.size() > 255) { error = "Expanded parameter name exceeds 255 characters"; return false; }
            }
            nodes[localId] = node.id;
            if (const auto* declaration = body.FindNode(localId); declaration->kind == NodeKind::SubgraphInput) inputs[localId] = PinId(node.id, 0);
            MaterialNodeOrigin origin{node.id, localId, call.assetGuid, prefix};
            if (const auto nested = bodyOrigins.find(localId); nested != bodyOrigins.end()) {
                origin.sourceNode = nested->second.sourceNode; origin.assetGuid = nested->second.assetGuid;
                origin.instancePath += nested->second.instancePath;
            }
            origins[node.id] = std::move(origin);
            graph.nodes.push_back(std::move(node));
        }
        const auto output = [&](std::uint32_t pin) {
            for (std::size_t hops = 0; hops <= body.nodes.size(); ++hops) {
                const auto* node = body.FindNode(pin / 16);
                if (!node) return std::uint32_t{0};
                if (node->kind == NodeKind::SubgraphInput) return inputs.at(node->id);
                if (node->kind != NodeKind::SubgraphOutput) return PinId(nodes.at(node->id), pin % 16 - 1);
                pin = InputSource(body, PinId(node->id, 0));
            }
            return std::uint32_t{0};
        };
        for (std::size_t at = 0; at < call.ports.size(); ++at) if (!call.ports[at].input) {
            const auto source = output(PinId(call.ports[at].id, 1));
            if (!source) { error = "Unconnected subgraph output: " + call.ports[at].name; return false; }
            outputs[PinId(call.id, at)] = source;
        }
        for (auto& link : graph.links) if (link.output / 16 == call.id) link.output = outputs.at(link.output);
        for (auto& layer : graph.layers) if (layer.maskPin / 16 == call.id) layer.maskPin = outputs.at(layer.maskPin);
        if (preview / 16 == call.id) preview = outputs.at(preview);
        auto links = body.links;
        std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.input < b.input; });
        for (auto link : links) {
            if (body.FindNode(link.input / 16)->kind == NodeKind::SubgraphOutput) continue;
            link.output = output(link.output); link.input = PinId(nodes.at(link.input / 16), link.input % 16 - 1);
            if (!link.output || graph.links.size() >= 8192 || !Allocate(graph, link.id, error)) {
                if (error.empty()) error = "Expanded graph has an invalid source or exceeds 8192 links"; return false;
            }
            graph.links.push_back(link);
        }
        return true;
    }
    const MaterialSubgraphResolver& resolver_;
    struct ResolvedAsset { MaterialSubgraph asset; std::vector<MaterialGraphPort> ports; };
    std::unordered_map<std::string, ResolvedAsset> assets_;
    std::vector<std::string> stack_;
    unsigned calls_ = 0;
};
}
MaterialSubgraph MaterialSubgraph::Example()
{
    MaterialSubgraph asset; auto& graph = asset.graph; std::string error;
    const auto input = graph.AddNode(NodeKind::SubgraphInput, {0, 0});
    const auto scale = graph.AddNode(NodeKind::ScalarParameter, {0, 180});
    const auto multiply = graph.AddNode(NodeKind::Multiply, {300, 0});
    const auto output = graph.AddNode(NodeKind::SubgraphOutput, {600, 0});
    graph.FindNode(input)->parameter = "Value"; graph.FindNode(input)->value[0] = .5f;
    graph.FindNode(scale)->parameter = "Scale"; graph.FindNode(scale)->value[0] = 1;
    graph.FindNode(output)->parameter = "Result";
    graph.Connect(PinId(input, 0), PinId(multiply, 0), error);
    graph.Connect(PinId(scale, 0), PinId(multiply, 1), error);
    graph.Connect(PinId(multiply, 2), PinId(output, 0), error); return asset;
}
bool MaterialSubgraph::GetInterface(std::vector<MaterialGraphPort>& ports, std::string& error) const
{
    error.clear(); MaterialGraphDocument checked;
    if (!interfaceVersion || interfaceVersion > 65535 || !checked.FromJson(graph.ToJson(), error)) {
        if (error.empty()) error = "Invalid subgraph interface version"; return false;
    }
    if (checked.HasLayerStack() || std::any_of(checked.nodes.begin(), checked.nodes.end(), [](const auto& n) { return n.kind == NodeKind::PbrOutput; })) {
        error = "Subgraph assets cannot own a material output or Layer Stack"; return false;
    }
    std::vector<MaterialGraphPort> draft; std::unordered_set<std::string> names;
    for (const auto& node : checked.nodes) if (node.kind == NodeKind::SubgraphInput || node.kind == NodeKind::SubgraphOutput) {
        if (!names.insert(node.parameter).second) { error = "Duplicate subgraph interface name: " + node.parameter; return false; }
        if (node.kind == NodeKind::SubgraphOutput && !InputSource(checked, PinId(node.id, 0))) { error = "Unconnected subgraph output: " + node.parameter; return false; }
        draft.push_back({node.id, node.parameter, node.interfaceType, node.kind == NodeKind::SubgraphInput, node.value});
    }
    if (draft.size() > 14 || std::none_of(draft.begin(), draft.end(), [](const auto& p) { return !p.input; })) {
        error = "Subgraph requires an output and at most 14 interface ports"; return false;
    }
    std::sort(draft.begin(), draft.end(), [](const auto& a, const auto& b) { return a.input != b.input ? a.input > b.input : a.id < b.id; });
    ports = std::move(draft); return true;
}
std::string MaterialSubgraph::ToJson() const
{
    Json root(crude_json::type_t::object); root["format"] = "ixtreeme.materialsubgraph"; root["version"] = 1.0;
    root["interface_version"] = static_cast<double>(interfaceVersion); root["graph"] = Json::parse(graph.ToJson()); return root.dump(2);
}
bool MaterialSubgraph::FromJson(const std::string& text, std::string& error)
{
    error.clear(); if (text.size() > 4 * 1024 * 1024) { error = "Subgraph file exceeds 4 MB"; return false; }
    const auto root = Json::parse(text);
    if (!root.is_object() || !root.contains("format") || !root["format"].is_string() || root["format"].get<crude_json::string>() != "ixtreeme.materialsubgraph" ||
        !root.contains("version") || !root["version"].is_number() || root["version"].get<crude_json::number>() != 1 ||
        !root.contains("interface_version") || !root["interface_version"].is_number() || !root.contains("graph")) {
        error = "Invalid or unsupported material subgraph format"; return false;
    }
    const double version = root["interface_version"].get<crude_json::number>();
    if (!std::isfinite(version) || version < 1 || version > 65535 || std::floor(version) != version) { error = "Invalid subgraph interface version"; return false; }
    MaterialSubgraph draft; draft.interfaceVersion = static_cast<std::uint32_t>(version);
    if (!draft.graph.FromJson(root["graph"].dump(), error)) return false;
    std::vector<MaterialGraphPort> ports; if (!draft.GetInterface(ports, error)) return false;
    *this = std::move(draft); return true;
}
bool BindMaterialSubgraph(MaterialGraphDocument& document, std::uint32_t id,
                         const std::string& guid, const MaterialSubgraph& asset, std::string& error)
{
    error.clear(); const auto* original = document.FindNode(id);
    if (!original || original->kind != NodeKind::Subgraph || original->region != GraphRegion::Custom || !IsMaterialAssetGuid(guid)) {
        error = "Invalid subgraph node or GUID"; return false;
    }
    std::vector<MaterialGraphPort> ports; if (!asset.GetInterface(ports, error)) return false;
    const auto remap = [&](std::uint32_t pin) {
        const auto slot = pin % 16;
        if (!slot || slot > original->ports.size()) return std::uint32_t{0};
        const auto& old = original->ports[slot - 1];
        for (std::size_t at = 0; at < ports.size(); ++at) if (ports[at].id == old.id && ports[at].type == old.type && ports[at].input == old.input) return PinId(id, at);
        return std::uint32_t{0};
    };
    auto draft = document;
    for (auto& link : draft.links) for (auto* pin : {&link.input, &link.output}) if (*pin / 16 == id) {
        *pin = remap(*pin); if (!*pin) { error = "Cannot refresh a connected removed or type-changed subgraph port"; return false; }
    }
    for (auto& layer : draft.layers) if (layer.maskPin / 16 == id) {
        layer.maskPin = remap(layer.maskPin); if (!layer.maskPin) { error = "Cannot refresh a connected layer mask port"; return false; }
    }
    auto& node = *draft.FindNode(id); node.assetGuid = guid; node.interfaceVersion = asset.interfaceVersion; node.ports = std::move(ports);
    MaterialGraphDocument checked; if (!checked.FromJson(draft.ToJson(), error)) return false;
    document = std::move(draft); return true;
}
bool ExpandMaterialSubgraphs(const MaterialGraphDocument& document, const MaterialSubgraphResolver& resolver,
                             MaterialGraphDocument& expanded, MaterialSubgraphExpansion& info,
                             std::string& error, std::uint32_t previewPin)
{
    error.clear(); MaterialGraphDocument draft; if (!draft.FromJson(document.ToJson(), error)) return false;
    if (previewPin) {
        const auto pin = draft.FindPin(previewPin);
        if (!pin || pin->definition.input || pin->definition.type == PinType::Texture) { error = "Invalid subgraph preview output"; return false; }
    }
    Expander expander(resolver); Origins origins;
    if (!expander.Calls(draft, origins, error, previewPin)) return false;
    // Authoring a standalone subgraph uses declared numeric defaults for its inputs.
    // Texture/Surface inputs require a caller; they cannot invent preview resources.
    for (auto& node : draft.nodes) if (node.kind == NodeKind::SubgraphInput) {
        const auto kind = ConstantKind(node.interfaceType);
        if (!kind) { error = "Standalone subgraph preview requires bound Texture/Surface inputs"; return false; }
        node.kind = *kind; node.parameter.clear();
    }
    const auto alias = [&](std::uint32_t pin) {
        for (std::size_t hops = 0; hops <= draft.nodes.size(); ++hops) {
            const auto* node = draft.FindNode(pin / 16);
            if (!node || node->kind != NodeKind::SubgraphOutput) return pin;
            pin = InputSource(draft, PinId(node->id, 0)); if (!pin) return std::uint32_t{0};
        }
        return std::uint32_t{0};
    };
    // Resolve all aliases before changing links, so declaration chains are order-independent.
    std::unordered_map<std::uint32_t, std::uint32_t> aliases;
    for (const auto& node : draft.nodes) if (node.kind == NodeKind::SubgraphOutput) {
        const auto pin = PinId(node.id, 1), source = alias(pin);
        if (!source) { error = "Unconnected subgraph output: " + node.parameter; return false; }
        aliases[pin] = source;
    }
    if (!previewPin && std::none_of(draft.nodes.begin(), draft.nodes.end(), [](const auto& n) { return n.kind == NodeKind::PbrOutput; }) && !aliases.empty())
        previewPin = std::min_element(aliases.begin(), aliases.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->second;
    else if (aliases.contains(previewPin)) previewPin = aliases.at(previewPin);
    std::erase_if(draft.links, [&](const auto& l) { return draft.FindNode(l.input / 16)->kind == NodeKind::SubgraphOutput; });
    for (auto& link : draft.links) if (aliases.contains(link.output)) link.output = aliases.at(link.output);
    for (auto& layer : draft.layers) if (aliases.contains(layer.maskPin)) layer.maskPin = aliases.at(layer.maskPin);
    std::erase_if(draft.nodes, [](const auto& n) { return n.kind == NodeKind::SubgraphOutput; });
    MaterialGraphDocument checked; if (!checked.FromJson(draft.ToJson(), error)) return false;
    MaterialSubgraphExpansion details; details.previewPin = previewPin;
    details.dependencyGuids.assign(expander.dependencies.begin(), expander.dependencies.end());
    std::sort(details.dependencyGuids.begin(), details.dependencyGuids.end());
    for (const auto& [id, origin] : origins) { (void)id; details.origins.push_back(origin); }
    expanded = std::move(draft); info = std::move(details); return true;
}
}
