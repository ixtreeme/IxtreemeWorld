#pragma once
#include "MaterialGraphDocument.h"
#include "MaterialSubgraph.h"
#include <functional>
#include <unordered_map>

namespace material_tool
{
using Float4 = std::array<float, 4>;
struct Surface
{
    Float4 color{0.5f, 0.5f, 0.5f, 1}, normal{0, 0, 1, 0}, emissive{};
    float roughness = 0.5f, metallic = 0, ao = 1, height = 0, opacity = 1;
};
struct EvaluationContext
{
    Float4 uv{}, worldPosition{}, worldNormal{0, 1, 0, 0}, viewDirection{0, 0, 1, 0};
    float ao = 1, curvature = 0.5f, cavity = 0;
    float time = 0;        // seconds, for Time
    float sceneDepth = -1; // metres of scene behind the surface; negative: none (Scene Depth uses its fallback)
    bool operator==(const EvaluationContext&) const = default;
};
struct ParameterOverrides
{
    std::unordered_map<std::string, Float4> values;
    std::unordered_map<std::string, std::string> textures;
    bool operator==(const ParameterOverrides&) const = default;
};
struct MaterialParameter
{
    std::uint32_t nodeId = 0;
    std::string name;
    PinType type = PinType::Scalar;
    Float4 defaultValue{};
    // Numeric fields are float4-aligned and ordered by stable node ID. Texture fields
    // have no numeric offset. This is the HLSL source layout, not SPIR-V reflection.
    std::uint32_t byteOffset = 0;
};
struct MaterialTextureBinding
{
    std::uint32_t nodeId = 0, binding = 0;
    std::string resource, assetGuid, assetId, parameter;
    Float4 fallback{1,1,1,1};
};
struct MaterialCompileResult
{
    std::vector<MaterialParameter> parameters;
    std::vector<MaterialTextureBinding> textures;
    std::vector<std::string> dependencyGuids;
    std::vector<MaterialNodeOrigin> subgraphOrigins;
    std::uint32_t parameterBufferBytes = 0;
    bool worldDependent = false, needsGeometryData = false;
};
struct SampledTexture {
    Float4 rgba{1,1,1,1};
    bool linearRgb = false; // EXR/HDR sources already contain scene-linear RGB.
    SampledTexture() = default;
    SampledTexture(Float4 value, bool linear = false) : rgba(value), linearRgb(linear) {}
};
// Source owns decoding/cache and returns raw RGBA with bilinear repeat filtering.
// RGB decoding never changes the separate data-channel and normal outputs.
using TextureSampler = std::function<SampledTexture(const GraphNode&, const std::string&, float, float)>;
// Descriptor layout of a graph pixel shader drawn by the static mesh renderer (Vulkan target):
// one set holding a uniform block (numeric parameters in layout order, then one float4 per
// texture: x = 1 for linear RGB sources), the caller's geometry map, and each texture with its
// own bilinear repeat sampler at kGraphFirstTextureBinding + MaterialTextureBinding::binding.
constexpr std::uint32_t kGraphDescriptorSet = 1, kGraphUniformBinding = 0, kGraphGeometryBinding = 1, kGraphFirstTextureBinding = 2;
struct HlslOptions
{
    bool vulkanDescriptorSet = false;
    std::uint32_t descriptorSet = kGraphDescriptorSet;
};
struct GraphValue { Float4 number{}; Surface surface{}; std::uint32_t textureNode = 0; };
struct GraphInstruction
{
    GraphNode node;
    std::array<int, 14> sources{};
    std::array<int, 14> sourceSlots{};
    std::uint16_t numericOutputs = 0;
    std::uint16_t usedOutputs = 0; // output slots another instruction or the preview reads
};
class MaterialGraphProgram
{
public:
    bool Compile(const MaterialGraphDocument& document, std::string& error, std::uint32_t previewPin = 0,
                 const MaterialSubgraphResolver& subgraphs = {});
    // Workspace is caller-owned and reused over every texel: no per-pixel graph traversal/allocation.
    Surface Evaluate(const EvaluationContext& context, const ParameterOverrides& parameters,
                     const TextureSampler& sample, std::vector<std::array<GraphValue, 14>>& workspace) const;
    GraphValue PreviewValue(const std::vector<std::array<GraphValue, 14>>& workspace) const;
    std::string GenerateHlsl(const HlslOptions& options = {}) const;
    // The Vulkan uniform block: parameters with overrides applied, then each texture's info.
    bool BuildRuntimeUniforms(const ParameterOverrides& parameters, const std::vector<bool>& linearTextures,
                              std::vector<Float4>& block, std::string& error) const;
    const MaterialCompileResult& Layout() const { return layout_; }
    bool ValidateOverrides(const ParameterOverrides& parameters, std::string& error) const;
    bool BuildParameterBuffer(const ParameterOverrides& parameters, std::vector<Float4>& buffer, std::string& error) const;
    bool HasOutput() const { return output_ >= 0; }
    bool CanBakeUV() const { return !worldDependent_; }
    bool NeedsSurfaceData() const { return surfaceData_; }
    const std::vector<GraphInstruction>& Instructions() const { return instructions_; }
private:
    std::vector<GraphInstruction> instructions_;
    int output_ = -1, preview_ = -1, previewSlot_ = 0;
    bool worldDependent_ = false, surfaceData_ = false;
    MaterialCompileResult layout_;
};
Float4 Normalize3(Float4 value, Float4 fallback = {0, 0, 1, 0});
float LinearToSrgb(float value);
float SrgbToLinear(float value);
bool SameEvaluationGraph(const MaterialGraphDocument& a, const MaterialGraphDocument& b);
}
