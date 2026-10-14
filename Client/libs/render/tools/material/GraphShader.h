#pragma once
#include "MaterialGraphCompiler.h"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace material_tool
{
// The runtime form of a graph material: a pixel shader for the static mesh pipeline, compiled to
// SPIR-V when the material is published, with the values of its descriptor set (kGraphDescriptorSet).
// The renderer draws the material's lit colour with it; everything else (depth, shadows,
// impostors, unsupported paths) keeps using the material's baked textures.
struct GraphShaderPackage
{
    std::vector<std::uint32_t> opaqueSpirv; // PSMain without discard
    std::vector<std::uint32_t> maskedSpirv; // PSMain with coverage and alpha discard (mask, blend)
    std::vector<Float4> uniforms;           // kGraphUniformBinding, see BuildRuntimeUniforms
    struct Texture
    {
        std::uint32_t binding = 0; // kGraphGeometryBinding or kGraphFirstTextureBinding + n
        std::string guid;          // a texture asset, sampled through a UNORM view
        bool operator==(const Texture&) const = default;
    };
    std::vector<Texture> textures;
    bool operator==(const GraphShaderPackage&) const = default;
};
// Little-endian, checksummed; decoding validates every count and the SPIR-V headers and preserves
// the destination on failure.
bool EncodeGraphShaderPackage(const GraphShaderPackage& package, std::string& bytes, std::string& error);
bool DecodeGraphShaderPackage(const std::string& bytes, GraphShaderPackage& package, std::string& error);

struct SpirvBinding
{
    std::uint32_t set = 0, binding = 0;
    std::string name;
};
// Descriptor bindings actually present in a SPIR-V module (not the source's declarations).
bool ReflectSpirvBindings(const std::vector<std::uint32_t>& words, std::vector<SpirvBinding>& bindings, std::string& error);
// Accepts a graph shader only if every binding it uses is one the static mesh renderer provides:
// set 0 bindings 0..5 and, in kGraphDescriptorSet, the uniform block, the geometry map when the
// graph reads geometry data, and textureCount textures.
bool ValidateGraphShaderBindings(const std::vector<std::uint32_t>& words, std::size_t textureCount, bool geometryMap, std::string& error);

// Complete pixel shader source: the embedded static mesh header, the graph in Vulkan descriptor
// form, and PSMain. GRAPH_OPAQUE selects the variant without discard.
std::string BuildGraphPixelShader(const MaterialGraphProgram& program);
// Uniform blocks an editor pushes into published graph materials while their parameters are
// edited, without recompiling or rewriting anything: every renderer drawing the material picks
// them up for its next frame. A block whose size does not match the loaded shader is ignored.
class GraphLiveValues
{
public:
    static GraphLiveValues& Instance();
    void Set(const std::string& materialGuid, std::vector<Float4> uniforms);
    void Clear(const std::string& materialGuid);
    // The material's current revision (0: none); its values are copied when it differs from known.
    std::uint64_t Get(const std::string& materialGuid, std::uint64_t known, std::vector<Float4>& uniforms) const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::vector<Float4>>> values_;
    std::uint64_t next_ = 1;
};

// IX_DXC, then the Vulkan SDK, then the compiler this engine was built with, then dxc on PATH.
std::filesystem::path FindShaderCompiler();
// Compiles both variants (blocking: run off the UI thread when latency matters) and checks their
// reflected bindings. Only the SPIR-V fields of package are written.
bool CompileGraphShader(const MaterialGraphProgram& program, GraphShaderPackage& package, std::string& error,
                        const std::filesystem::path& compiler = {});
}
