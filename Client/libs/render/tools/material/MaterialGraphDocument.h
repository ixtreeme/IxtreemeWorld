#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace material_tool
{
enum class NodeKind {
    Scalar, Color, Multiply, Blend, PbrOutput,
    Vector2, Vector3, ScalarParameter, ColorParameter, TextureParameter,
    UV, TilingOffset, WorldPosition, WorldNormal, TextureSample, Triplanar,
    Add, Subtract, Divide, Minimum, Maximum, Power, Lerp, Clamp, Remap,
    Smoothstep, Saturate, OneMinus, Abs, ColorMultiply, ColorAdd,
    MakeColor, SplitColor, MakeVector, SplitVector, VectorScale, Dot,
    NormalBlend, Noise, HeightMask, SlopeMask, AO, Curvature, Cavity,
    MaterialLayer, LayerBlend, HeightBlend,
    NormalStrength, NormalUnpack, NormalFlipY, Normalize, Cross, Length, Distance,
    Sine, Cosine, Fract, Floor, Ceil, Step, Sign, Reflect,
    MaskCombine, MaskLevels, MaskContrast, MaskInvert, Fresnel,
    Vector2Parameter, Vector3Parameter,
    SubgraphInput, SubgraphOutput, Subgraph,
    // Scene inputs of a material drawn at run time: the time, the scene behind a transparent
    // surface and the sky. Baking and the editor preview use fixed stand-ins (time 0, the node's
    // fallback depth, a default sky).
    Time, SceneDepth, SkyReflection, Count
};
enum class PinType { Scalar, Color, Vector2, Vector3, Texture, Surface };
enum class GraphRegion { Custom, ManagedLayer, Output };
struct PinDefinition { const char* name; PinType type; bool input; };
struct MaterialGraphPort
{
    // Interface IDs are the stable declaration-node IDs in the referenced asset.
    std::uint32_t id = 0;
    std::string name;
    PinType type = PinType::Scalar;
    bool input = true;
    std::array<float, 4> defaultValue{};
    bool operator==(const MaterialGraphPort&) const = default;
};
struct GraphNode
{
    std::uint32_t id = 0;
    NodeKind kind = NodeKind::Scalar;
    std::string name;
    std::array<float, 4> value{0.5f, 0.5f, 0.5f, 1.0f};
    std::array<float, 2> position{};
    std::array<float, 4> settings{};
    std::string resource, assetGuid, parameter, assetId;
    bool srgb = true;
    GraphRegion region = GraphRegion::Custom;
    std::uint32_t owner = 0;
    PinType interfaceType = PinType::Scalar;
    std::uint32_t interfaceVersion = 1;
    std::vector<MaterialGraphPort> ports;
    bool operator==(const GraphNode&) const = default;
};
struct GraphLink
{
    std::uint32_t id = 0, output = 0, input = 0;
    bool operator==(const GraphLink&) const = default;
};
struct GraphPin
{
    std::uint32_t node = 0;
    PinDefinition definition;
};
struct MaterialLayer
{
    std::uint32_t id = 0, sourceNode = 0, opacityNode = 0, weightNode = 0, blendNode = 0, maskPin = 0;
    bool enabled = true;
    bool operator==(const MaterialLayer&) const = default;
};
const char* NodeKindName(NodeKind kind);
const char* NodeCategory(NodeKind kind);
bool IsSubgraphNode(NodeKind kind);
const char* PinTypeName(PinType type);
std::span<const NodeKind> AllNodeKinds();
std::span<const PinDefinition> NodePins(NodeKind kind);
// Dynamic declarations point into node strings; the node must outlive these definitions.
std::vector<PinDefinition> NodePins(const GraphNode& node);
// Disjoint UI IDs: node = id*16, pins = id*16+1.., link = id*16+15.
std::uint32_t PinId(std::uint32_t node, std::size_t slot);

// Owns graph content, independently of Dear ImGui and the node editor's layout state.
// The document and compiler are also available in runtime builds; no UI types are used.
struct MaterialGraphDocument
{
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    std::vector<MaterialLayer> layers;
    std::uint32_t nextId = 1;
    bool operator==(const MaterialGraphDocument&) const = default;
    static MaterialGraphDocument Example();
    static MaterialGraphDocument WeatheredMetal();
    static MaterialGraphDocument MossyRock();
    static MaterialGraphDocument WeatheredStone();
    GraphNode* FindNode(std::uint32_t id);
    const GraphNode* FindNode(std::uint32_t id) const;
    std::optional<GraphPin> FindPin(std::uint32_t id) const;
    std::uint32_t AddNode(NodeKind kind, std::array<float, 2> position);
    bool RemoveNode(std::uint32_t id, bool managedEdit = false);
    bool RemoveLink(std::uint32_t id, bool managedEdit = false);
    bool CanConnect(std::uint32_t first, std::uint32_t second, std::string& reason, bool managedEdit = false) const;
    bool Connect(std::uint32_t first, std::uint32_t second, std::string& reason, bool managedEdit = false);
    MaterialLayer* FindLayer(std::uint32_t id);
    const MaterialLayer* FindLayer(std::uint32_t id) const;
    bool HasLayerStack() const;
    std::uint32_t AddMaterialLayer(const std::string& name, std::string& error);
    bool RemoveMaterialLayer(std::uint32_t id, std::string& error);
    std::uint32_t DuplicateMaterialLayer(std::uint32_t id, std::string& error);
    bool MoveMaterialLayer(std::uint32_t id, std::size_t index, std::string& error);
    bool SetMaterialLayerEnabled(std::uint32_t id, bool enabled, std::string& error);
    bool SetMaterialLayerMask(std::uint32_t id, std::uint32_t maskPin, std::string& error);
    bool SetMaterialLayerHeightBlend(std::uint32_t id, bool heightBlend, std::string& error);
    // Explicit detachment keeps every node/link/value intact; editor snapshots make it undoable.
    void ConvertToCustomGraph();
    bool ValidateLayerStack(std::string& error) const;
    std::string ToJson() const;
    // On error leaves this document intact, including positions and ID allocation.
    bool FromJson(const std::string& text, std::string& error);
private:
    bool RebuildLayerLinks(std::string& error);
};
// One canonical definition. Existing APIs and .materialgraph documents retain their name.
using MaterialDefinition = MaterialGraphDocument;
}
