#include "GraphShader.h"
#include "GraphShaderSources.h"
#include "platform/process.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace material_tool
{
namespace
{
constexpr char kMagic[8] = {'I','X','G','S','H','D','0','1'};
constexpr std::uint32_t kVersion = 1, kSpirvMagic = 0x07230203U;
constexpr std::size_t kMaxSpirvWords = 4u << 20, kMaxUniforms = 4096, kMaxTextures = 64, kMaxGuid = 64;

std::uint64_t Fnv(const char* data, std::size_t size)
{
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) { h ^= static_cast<unsigned char>(data[i]); h *= 1099511628211ULL; }
    return h;
}
void PutU32(std::string& s, std::uint32_t v) { for (unsigned k = 0; k < 4; ++k) s += static_cast<char>((v >> (k * 8)) & 0xffU); }
void PutU64(std::string& s, std::uint64_t v) { PutU32(s, static_cast<std::uint32_t>(v)); PutU32(s, static_cast<std::uint32_t>(v >> 32)); }
struct Reader
{
    const std::string& data;
    std::size_t at = 0, end = 0;
    bool ok = true;
    std::uint32_t U32()
    {
        if (at + 4 > end) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (unsigned k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[at++])) << (k * 8);
        return v;
    }
    float F32() { return std::bit_cast<float>(U32()); }
};
bool ValidSpirv(const std::vector<std::uint32_t>& words)
{
    return words.size() >= 5 && words.size() <= kMaxSpirvWords && words[0] == kSpirvMagic;
}
std::string Environment(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr; std::size_t count = 0;
    if (_dupenv_s(&value, &count, name) != 0 || !value) return {};
    std::string result = value; std::free(value); return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}
bool WriteText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}
bool ReadWords(const std::filesystem::path& path, std::vector<std::uint32_t>& words)
{
    std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size % 4 || size / 4 > kMaxSpirvWords) return false;
    std::ifstream file(path, std::ios::binary);
    words.assign(static_cast<std::size_t>(size / 4), 0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size)));
}
std::string FirstLines(const std::string& text, std::size_t lines)
{
    std::istringstream in(text); std::string line, result;
    for (std::size_t n = 0; n < lines && std::getline(in, line); ++n) { if (!result.empty()) result += '\n'; result += line; }
    return result;
}
}

bool EncodeGraphShaderPackage(const GraphShaderPackage& package, std::string& bytes, std::string& error)
{
    if (!ValidSpirv(package.opaqueSpirv) || !ValidSpirv(package.maskedSpirv)) { error = "Graph shader package needs both SPIR-V variants"; return false; }
    if (package.uniforms.size() > kMaxUniforms || package.textures.size() > kMaxTextures) { error = "Graph shader package exceeds its limits"; return false; }
    std::string out(kMagic, sizeof(kMagic));
    PutU32(out, kVersion);
    PutU32(out, static_cast<std::uint32_t>(package.uniforms.size()));
    for (const auto& value : package.uniforms) for (float f : value) PutU32(out, std::bit_cast<std::uint32_t>(f));
    PutU32(out, static_cast<std::uint32_t>(package.textures.size()));
    for (const auto& texture : package.textures)
    {
        if (texture.guid.empty() || texture.guid.size() > kMaxGuid) { error = "Graph shader texture needs an asset GUID"; return false; }
        PutU32(out, texture.binding);
        PutU32(out, static_cast<std::uint32_t>(texture.guid.size()));
        out += texture.guid;
    }
    for (const auto* spirv : {&package.opaqueSpirv, &package.maskedSpirv})
    {
        PutU32(out, static_cast<std::uint32_t>(spirv->size()));
        for (const auto word : *spirv) PutU32(out, word);
    }
    PutU64(out, Fnv(out.data(), out.size()));
    bytes = std::move(out); error.clear(); return true;
}

bool DecodeGraphShaderPackage(const std::string& bytes, GraphShaderPackage& package, std::string& error)
{
    if (bytes.size() < sizeof(kMagic) + 8 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) { error = "Not a graph shader package"; return false; }
    const std::size_t payload = bytes.size() - 8;
    Reader tail{bytes, payload, bytes.size()};
    const std::uint64_t stored = tail.U32() | (static_cast<std::uint64_t>(tail.U32()) << 32);
    if (stored != Fnv(bytes.data(), payload)) { error = "Graph shader package checksum mismatch"; return false; }
    Reader r{bytes, sizeof(kMagic), payload};
    if (r.U32() != kVersion) { error = "Unsupported graph shader package version"; return false; }
    GraphShaderPackage draft;
    const auto uniforms = r.U32();
    if (!r.ok || uniforms > kMaxUniforms) { error = "Invalid graph shader uniform count"; return false; }
    draft.uniforms.resize(uniforms);
    for (auto& value : draft.uniforms) for (float& f : value) f = r.F32();
    const auto textures = r.U32();
    if (!r.ok || textures > kMaxTextures) { error = "Invalid graph shader texture count"; return false; }
    std::set<std::uint32_t> bindings;
    for (std::uint32_t i = 0; i < textures; ++i)
    {
        GraphShaderPackage::Texture texture;
        texture.binding = r.U32();
        const auto length = r.U32();
        if (!r.ok || length == 0 || length > kMaxGuid || r.at + length > r.end) { error = "Invalid graph shader texture"; return false; }
        texture.guid.assign(bytes, r.at, length); r.at += length;
        if (texture.binding < kGraphGeometryBinding || !bindings.insert(texture.binding).second) { error = "Invalid graph shader texture binding"; return false; }
        draft.textures.push_back(std::move(texture));
    }
    for (auto* spirv : {&draft.opaqueSpirv, &draft.maskedSpirv})
    {
        const auto words = r.U32();
        if (!r.ok || words > kMaxSpirvWords || r.at + static_cast<std::size_t>(words) * 4 > r.end) { error = "Invalid graph shader SPIR-V size"; return false; }
        spirv->resize(words);
        for (auto& word : *spirv) word = r.U32();
        if (!ValidSpirv(*spirv)) { error = "Invalid graph shader SPIR-V"; return false; }
    }
    if (!r.ok || r.at != r.end) { error = "Graph shader package has trailing or missing data"; return false; }
    for (const auto& value : draft.uniforms) for (float f : value) if (!std::isfinite(f)) { error = "Graph shader package has invalid uniform values"; return false; }
    package = std::move(draft); error.clear(); return true;
}

bool ReflectSpirvBindings(const std::vector<std::uint32_t>& words, std::vector<SpirvBinding>& bindings, std::string& error)
{
    if (!ValidSpirv(words)) { error = "Not a SPIR-V module"; return false; }
    constexpr std::uint32_t OpName = 5, OpVariable = 59, OpDecorate = 71, Binding = 33, DescriptorSet = 34;
    std::map<std::uint32_t, std::string> names;
    std::map<std::uint32_t, std::uint32_t> sets, slots;
    std::set<std::uint32_t> variables;
    for (std::size_t at = 5; at < words.size();)
    {
        const auto count = words[at] >> 16, op = words[at] & 0xffffU;
        if (count == 0 || at + count > words.size()) { error = "Truncated SPIR-V instruction"; return false; }
        if (op == OpName && count >= 3)
        {
            const char* text = reinterpret_cast<const char*>(&words[at + 2]);
            names[words[at + 1]] = std::string(text, strnlen(text, (count - 2) * 4));
        }
        else if (op == OpDecorate && count >= 4 && words[at + 2] == DescriptorSet) sets[words[at + 1]] = words[at + 3];
        else if (op == OpDecorate && count >= 4 && words[at + 2] == Binding) slots[words[at + 1]] = words[at + 3];
        else if (op == OpVariable && count >= 4) variables.insert(words[at + 2]);
        at += count;
    }
    std::vector<SpirvBinding> result;
    for (const auto id : variables)
    {
        const auto set = sets.find(id), slot = slots.find(id);
        if (set == sets.end() || slot == slots.end()) continue;
        const auto name = names.find(id);
        result.push_back({set->second, slot->second, name == names.end() ? std::string{} : name->second});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.set != b.set ? a.set < b.set : a.binding < b.binding; });
    bindings = std::move(result); return true;
}

bool ValidateGraphShaderBindings(const std::vector<std::uint32_t>& words, std::size_t textureCount, bool geometryMap, std::string& error)
{
    std::vector<SpirvBinding> bindings;
    if (!ReflectSpirvBindings(words, bindings, error)) return false;
    for (const auto& b : bindings)
    {
        const bool staticMesh = b.set == 0 && b.binding <= 6;
        const bool graph = b.set == kGraphDescriptorSet && (b.binding == kGraphUniformBinding ||
            (geometryMap && b.binding == kGraphGeometryBinding) ||
            (b.binding >= kGraphFirstTextureBinding && b.binding < kGraphFirstTextureBinding + textureCount));
        if (!staticMesh && !graph)
        {
            error = "Graph shader uses descriptor set " + std::to_string(b.set) + " binding " + std::to_string(b.binding) +
                (b.name.empty() ? std::string{} : " (" + b.name + ")") + ", which the static mesh renderer does not provide";
            return false;
        }
    }
    error.clear(); return true;
}

std::string BuildGraphPixelShader(const MaterialGraphProgram& program)
{
    const bool geometry = program.Layout().needsGeometryData;
    HlslOptions options; options.vulkanDescriptorSet = true;
    std::ostringstream h;
    h << "#include \"StaticMeshCommon.hlsli\"\n" << program.GenerateHlsl(options);
    if (geometry)
        h << "[[vk::combinedImageSampler]] [[vk::binding(" << kGraphGeometryBinding << ", " << kGraphDescriptorSet << ")]] Texture2D<float4> graphGeometry;\n"
          << "[[vk::combinedImageSampler]] [[vk::binding(" << kGraphGeometryBinding << ", " << kGraphDescriptorSet << ")]] SamplerState graphGeometrySampler;\n";
    h << R"HLSL(
// The static mesh pixel shader with the graph as its surface: the same coverage, alpha test and
// lighting as StaticMesh.hlsl, the material values evaluated per pixel.
float4 PSMain(VSOutput input) : SV_Target0
{
#if !defined(GRAPH_OPAQUE)
    ApplyLodCoverage(input);
    if (u_lightPadding.x > 0.5 && input.worldPos.y < u_lightPadding.y)
        discard;
#endif
    const float3 geometricNormal = normalize(input.normal);
    GraphContext c;
    c.uv = input.uv;
    c.worldPosition = input.worldPos;
    c.worldNormal = geometricNormal;
    c.viewDirection = normalize(u_cameraPosition.xyz - input.worldPos);
    c.ao = 1.0; c.curvature = 0.5; c.cavity = 0.0;
)HLSL";
    if (geometry)
        h << "    const float3 geometryMap = graphGeometry.Sample(graphGeometrySampler, input.uv).rgb;\n"
             "    c.ao = geometryMap.r; c.curvature = geometryMap.g; c.cavity = geometryMap.b;\n";
    h << R"HLSL(    const GraphSurface s = EvaluateMaterialGraph(c);
    const float alpha = s.opacity * input.materialBaseColor.a * input.tint.a;
#if !defined(GRAPH_OPAQUE)
    if (input.materialAlpha.x > 0.5 && input.materialAlpha.x < 1.5 && alpha < input.materialAlpha.y)
        discard;
#endif
    const float3 albedo = s.color.rgb * input.materialBaseColor.rgb * input.tint.rgb;
    const float3 normal = PerturbNormal(geometricNormal, input.worldPos, input.uv, GraphNormalize(s.normal));
    const float3 color = StaticMeshLighting(albedo, normal, geometricNormal, input.worldPos, s.roughness, s.metallic, s.ao) + s.emissive;
    return float4(color, alpha);
}
)HLSL";
    return h.str();
}

GraphShaderJob::~GraphShaderJob() { if (worker_.joinable()) worker_.join(); }
bool GraphShaderJob::Start(MaterialGraphProgram program, GraphShaderPackage package)
{
    if (running_) return false;
    if (worker_.joinable()) worker_.join();
    ready_ = false; success_ = false; error_.clear(); result_ = {}; running_ = true;
    worker_ = std::jthread([this, program = std::move(program), package = std::move(package)]() mutable {
        try { success_ = CompileGraphShader(program, package, error_); if (success_) result_ = std::move(package); }
        catch (const std::exception& e) { success_ = false; error_ = std::string("Graph shader compilation failed: ") + e.what(); }
        ready_ = true; running_ = false;
    });
    return true;
}
bool GraphShaderJob::TakeResult(GraphShaderPackage& package, std::string& error)
{
    if (!ready_.load() || running_.load()) return false;
    if (worker_.joinable()) worker_.join();
    ready_ = false; error = error_;
    if (success_) package = std::move(result_);
    return true;
}
GraphLiveValues& GraphLiveValues::Instance()
{
    static GraphLiveValues instance;
    return instance;
}
void GraphLiveValues::Set(const std::string& materialGuid, std::vector<Float4> uniforms)
{
    std::lock_guard lock(mutex_);
    values_[materialGuid] = {next_++, std::move(uniforms)};
}
void GraphLiveValues::Clear(const std::string& materialGuid)
{
    std::lock_guard lock(mutex_);
    values_.erase(materialGuid);
}
std::uint64_t GraphLiveValues::Get(const std::string& materialGuid, std::uint64_t known, std::vector<Float4>& uniforms) const
{
    std::lock_guard lock(mutex_);
    const auto it = values_.find(materialGuid);
    if (it == values_.end()) return 0;
    if (it->second.first != known) uniforms = it->second.second;
    return it->second.first;
}

std::filesystem::path FindShaderCompiler()
{
    std::error_code ec;
    if (const auto configured = Environment("IX_DXC"); !configured.empty() && std::filesystem::is_regular_file(configured, ec))
        return configured;
#if defined(_WIN32)
    constexpr const char* executable = "dxc.exe";
#else
    constexpr const char* executable = "dxc";
#endif
    if (const auto sdk = Environment("VULKAN_SDK"); !sdk.empty())
        for (const char* folder : {"Bin", "bin"})
            if (const auto candidate = std::filesystem::path(sdk) / folder / executable; std::filesystem::is_regular_file(candidate, ec))
                return candidate;
#if defined(IX_GRAPH_SHADER_COMPILER)
    if (const std::filesystem::path built = IX_GRAPH_SHADER_COMPILER; std::filesystem::is_regular_file(built, ec))
        return built;
#endif
    return executable; // resolved through PATH when launched
}

bool CompileGraphShader(const MaterialGraphProgram& program, GraphShaderPackage& package, std::string& error,
                        const std::filesystem::path& compiler)
{
    if (!program.HasOutput()) { error = "The graph needs a PBR Output to become a shader"; return false; }
    const auto dxc = compiler.empty() ? FindShaderCompiler() : compiler;
    std::random_device random;
    const auto folder = std::filesystem::temp_directory_path() / ("ix-graph-shader-" + std::to_string(random()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    if (!std::filesystem::create_directories(folder, ec)) { error = "Cannot create a folder for shader compilation"; return false; }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{folder};
    const auto source = folder / "graph.hlsl";
    if (!WriteText(folder / "StaticMeshCommon.hlsli", graph_shader_sources::staticMeshCommon) ||
        !WriteText(folder / "SunShadow.hlsli", graph_shader_sources::sunShadow) ||
        !WriteText(source, BuildGraphPixelShader(program)))
    { error = "Cannot write the graph shader source"; return false; }
    const std::size_t textures = program.Layout().textures.size();
    const bool geometry = program.Layout().needsGeometryData;
    std::vector<std::uint32_t> variants[2];
    for (int variant = 0; variant < 2; ++variant)
    {
        const auto output = folder / (variant == 0 ? "opaque.spv" : "masked.spv");
        std::vector<std::string> argv = {dxc.string(), "-spirv", "-T", "ps_6_0", "-E", "PSMain", "-I", folder.string()};
        if (variant == 0) { argv.push_back("-D"); argv.push_back("GRAPH_OPAQUE=1"); }
        argv.insert(argv.end(), {"-Fo", output.string(), source.string()});
        const auto result = platform::RunProcess(argv, folder);
        if (!result.launched) { error = "Shader compiler not found (" + dxc.string() + "); install the Vulkan SDK or set IX_DXC"; return false; }
        if (result.exitCode != 0 || !ReadWords(output, variants[variant]))
        { error = "Graph shader compilation failed: " + FirstLines(result.output, 12); return false; }
        if (!ValidateGraphShaderBindings(variants[variant], textures, geometry, error)) return false;
    }
    package.opaqueSpirv = std::move(variants[0]);
    package.maskedSpirv = std::move(variants[1]);
    error.clear(); return true;
}
}
