#include "tools/material/GraphShader.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace material_tool;
namespace {
int failures = 0;
void Check(bool ok, const std::string& what) { std::cout << (ok ? "PASS " : "FAIL ") << what << '\n'; failures += !ok; }
bool HasBinding(const std::vector<SpirvBinding>& bindings, std::uint32_t set, std::uint32_t binding)
{
    return std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) { return b.set == set && b.binding == binding; });
}
}

int main()
{
    struct Preset { const char* name; MaterialGraphDocument document; };
    const Preset presets[] = {{"Example", MaterialGraphDocument::Example()}, {"Weathered Metal", MaterialGraphDocument::WeatheredMetal()},
        {"Mossy Rock", MaterialGraphDocument::MossyRock()}, {"Weathered Stone", MaterialGraphDocument::WeatheredStone()}};
    std::error_code ec;
#if defined(IX_GRAPH_DXC)
    const std::filesystem::path compiler = IX_GRAPH_DXC;
#else
    const std::filesystem::path compiler = FindShaderCompiler();
#endif
    const bool haveCompiler = std::filesystem::is_regular_file(compiler, ec);
    if (!haveCompiler) std::cout << "SKIP no shader compiler: SPIR-V compilation and reflection are not tested\n";
    GraphShaderPackage compiled;
    std::size_t compiledTextures = 0;
    for (const auto& preset : presets)
    {
        MaterialGraphProgram program; std::string error;
        Check(program.Compile(preset.document, error), std::string(preset.name) + " compiles to graph IR: " + error);
        const auto source = BuildGraphPixelShader(program);
        const auto& layout = program.Layout();
        Check(source.find("#include \"StaticMeshCommon.hlsli\"") == 0 && source.find("float4 PSMain(VSOutput input)") != std::string::npos,
              std::string(preset.name) + " source uses the static mesh header and entry");
        Check(source.find(": register(") == std::string::npos, std::string(preset.name) + " runtime source has no implicit register bindings");
        Check((source.find("graphGeometry") != std::string::npos) == layout.needsGeometryData,
              std::string(preset.name) + " declares the geometry map exactly when it reads geometry data");
        std::vector<Float4> uniforms;
        Check(program.BuildRuntimeUniforms({}, std::vector<bool>(layout.textures.size(), false), uniforms, error) &&
              uniforms.size() == layout.parameterBufferBytes / sizeof(Float4) + layout.textures.size(),
              std::string(preset.name) + " uniform block holds every parameter and texture info");
        if (!haveCompiler) continue;
        GraphShaderPackage package;
        const bool built = CompileGraphShader(program, package, error, compiler);
        if (!built) std::cout << error << '\n';
        Check(built, std::string(preset.name) + " compiles to SPIR-V in both variants");
        if (!built) continue;
        std::vector<SpirvBinding> bindings;
        Check(ReflectSpirvBindings(package.maskedSpirv, bindings, error) && HasBinding(bindings, 0, 0) && HasBinding(bindings, 0, 5),
              std::string(preset.name) + " reads the static mesh constants and shadow map");
        Check(HasBinding(bindings, kGraphDescriptorSet, kGraphUniformBinding) == !uniforms.empty(),
              std::string(preset.name) + " uniform block is bound exactly when it has values");
        Check(HasBinding(bindings, kGraphDescriptorSet, kGraphGeometryBinding) == layout.needsGeometryData,
              std::string(preset.name) + " geometry map binding matches its geometry needs");
        std::vector<SpirvBinding> opaque;
        Check(ReflectSpirvBindings(package.opaqueSpirv, opaque, error) && package.opaqueSpirv != package.maskedSpirv,
              std::string(preset.name) + " opaque variant is a distinct module");
        if (layout.needsGeometryData && compiled.opaqueSpirv.empty()) { compiled = package; compiledTextures = layout.textures.size(); }
    }

    if (haveCompiler && !compiled.opaqueSpirv.empty())
    {
        std::string error;
        // A module that reaches outside the renderer's layout is refused.
        auto foreign = compiled.maskedSpirv;
        bool rewritten = false;
        for (std::size_t at = 5; at < foreign.size() && !rewritten;)
        {
            const auto count = foreign[at] >> 16, op = foreign[at] & 0xffffU;
            if (op == 71 && count >= 4 && foreign[at + 2] == 34 && foreign[at + 3] == kGraphDescriptorSet) { foreign[at + 3] = 3; rewritten = true; }
            at += count ? count : 1;
        }
        Check(rewritten && !ValidateGraphShaderBindings(foreign, compiledTextures, true, error) && error.find("set 3") != std::string::npos,
              "a binding outside the static mesh and graph sets is rejected");
        Check(!ValidateGraphShaderBindings(compiled.maskedSpirv, compiledTextures, false, error), "an undeclared geometry map binding is rejected");

        compiled.uniforms = {{1, 2, 3, 4}, {.5f, 0, 0, 0}};
        compiled.textures = {{kGraphGeometryBinding, "8d1f5a3c-0000-4000-8000-000000000001"}, {kGraphFirstTextureBinding, "8d1f5a3c-0000-4000-8000-000000000002"}};
        std::string bytes; GraphShaderPackage decoded;
        Check(EncodeGraphShaderPackage(compiled, bytes, error) && DecodeGraphShaderPackage(bytes, decoded, error) && decoded == compiled,
              "package round-trips exactly: " + error);
        GraphShaderPackage kept = decoded;
        auto corrupt = bytes; corrupt[corrupt.size() / 2] ^= 0x5a;
        Check(!DecodeGraphShaderPackage(corrupt, kept, error) && kept == decoded, "corrupt package is rejected and preserves the destination");
        Check(!DecodeGraphShaderPackage(bytes.substr(0, bytes.size() - 9), kept, error), "truncated package is rejected");
        auto badMagic = bytes; badMagic[0] = 'X';
        Check(!DecodeGraphShaderPackage(badMagic, kept, error), "foreign file is rejected");
        auto duplicate = compiled; duplicate.textures[1].binding = kGraphGeometryBinding;
        Check(EncodeGraphShaderPackage(duplicate, bytes, error) && !DecodeGraphShaderPackage(bytes, kept, error), "duplicate texture bindings are rejected");
        auto missing = compiled; missing.maskedSpirv.clear();
        Check(!EncodeGraphShaderPackage(missing, bytes, error), "a package needs both shader variants");
    }
    std::cout << "Graph shader failures: " << failures << '\n';
    return failures ? 1 : 0;
}
