#include "asset/ExrImage.h"
#include "StaticMeshRenderer.h"

#include "AssimpImporter.h"
#include "import_export/tree/TreeImpostor.h"
#include "Debug.h"
#include "IXRHIShader.h"
#include "JobSystem.h"
#include "MaterialAssetManager.h"
#include "ProjectManager.h"
#include "asset/IAssetReader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>

#include <meshoptimizer.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace xm = ixtreeme::math;

using RgbaImage = StaticMeshRenderer::RgbaImage;

void LogFormat(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (!LodLogsEnabled() && std::strncmp(buffer, "[LOD", 4) == 0)
        return;
    Tracen(buffer);
}

using Mat4 = ixtreeme::math::Mat4;

struct UniformBlock
{
    struct PointLightUniform
    {
        float position[4];
        float color[4];
    };

    struct SpotLightUniform
    {
        float position[4];
        float direction[4];
        float color[4];
    };

    Mat4 mvp;
    Mat4 model;
    float tint[4];
    float materialBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float materialParams[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // metallic, roughness, normal strength, AO strength
    float materialEmissive[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float materialUv[4] = {1.0f, 1.0f, 0.0f, 0.0f}; // tiling.xy, offset.xy
    float materialAlpha[4] = {0.0f, 0.5f, 0.0f, 1.0f}; // mode, cutoff, coverage interval
    float cameraPosition[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float sunDir[4];
    float sunColor[4];
    float ambientColor[4];
    float waterParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float causticParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::int32_t numPointLights = 0;
    std::int32_t numSpotLights = 0;
    float lightPadding[2] = {0.0f, 0.0f};
    PointLightUniform pointLights[kMaxDynamicPointLights]{};
    SpotLightUniform spotLights[kMaxDynamicSpotLights]{};
    float shadowCascadeViewProj[SunShadowReceive::kCascades][16]{};
    float shadowParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float shadowDepthBias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float shadowNormalOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Bind sets' UniformBlocks share a page's buffer this far apart: a multiple of 256 bytes, the most
// any device asks uniform buffer offsets to be aligned to.
constexpr std::uint64_t kUniformStride = (sizeof(UniformBlock) + 255u) & ~std::uint64_t{255u};

struct InstancedDrawCommand
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t materialSlot = 0;
    uint32_t sourceSubmesh = 0;
};

// Diagnostic object identity for log lines (replaces raw VkHandleValue dumps;
// values are IXRHI object addresses, stable within a run).
template <typename T>
unsigned long long RhiObjectId(const std::shared_ptr<T>& object)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object.get()));
}

inline unsigned long long RhiObjectId(const void* object)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object));
}

// Loads SPIR-V words via the asset reader (aborts like the old shader loader
// when the asset is missing or misaligned).
std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        LogFormat("[STATIC-MESH] failed to open shader: %s", path.c_str());
        std::abort();
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes->data());
    return std::vector<std::uint32_t>(words, words + bytes->size() / sizeof(std::uint32_t));
}

std::shared_ptr<ixrhi::IXRHIShader> LoadShader(ixrhi::IXRHIDevice& rhi,
                                               client::asset::IAssetReader& assets,
                                               const std::string& path,
                                               ixrhi::IXRHIShaderStage stage,
                                               const char* entry)
{
    ixrhi::IXRHIShaderDesc desc;
    desc.stage = stage;
    desc.entryPoint = entry;
    desc.spirv = ReadSpirv(assets, path);
    desc.debugName = path;
    return rhi.CreateShader(desc);
}

std::shared_ptr<ixrhi::IXRHIBuffer> CreateRhiBuffer(ixrhi::IXRHIDevice& rhi,
                                                    std::uint64_t sizeBytes,
                                                    ixrhi::IXRHIBufferUsage usage,
                                                    ixrhi::IXRHICpuAccess cpuAccess,
                                                    const void* initialData,
                                                    const char* debugName)
{
    ixrhi::IXRHIBufferDesc desc;
    desc.sizeBytes = sizeBytes;
    desc.usage = usage;
    desc.cpuAccess = cpuAccess;
    desc.debugName = debugName ? debugName : "";
    const std::size_t bytes = static_cast<std::size_t>(sizeBytes);
    return rhi.CreateBuffer(desc, initialData, initialData != nullptr ? bytes : 0);
}

const char* AlphaModeForLog(const std::string& alphaMode)
{
    if (alphaMode == "mask")
        return "mask";
    if (alphaMode == "blend")
        return "blend";
    return "opaque";
}

template <typename T>
bool ReadBinary(std::istream& in, T& value)
{
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

template <typename T>
void WriteBinary(std::ostream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

const char* LodSourceName(StaticMeshRenderer::LodBufferSource source)
{
    switch (source)
    {
    case StaticMeshRenderer::LodBufferSource::Preview: return "preview";
    case StaticMeshRenderer::LodBufferSource::Cache: return "cache";
    case StaticMeshRenderer::LodBufferSource::Commit: return "commit";
    default: return "unknown";
    }
}

Mat4 Identity()
{
    return xm::Mat4Identity();
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    return xm::MultiplyRowMajor(a, b);
}

Mat4 Scale(float x, float y, float z)
{
    return xm::Scale({x, y, z});
}

Mat4 RotationX(float angle)
{
    return xm::RotationX(angle);
}

Mat4 RotationY(float angle)
{
    return xm::RotationYRowMajor(angle);
}

Mat4 RotationZ(float angle)
{
    return xm::RotationZ(angle);
}

Mat4 Translation(float x, float y, float z)
{
    return xm::Translation({x, y, z});
}

Mat4 ToLocalMat4(const WorldMat4& matrix)
{
    Mat4 r{};
    std::memcpy(r.m, matrix.m, sizeof(r.m));
    return r;
}

void FillLightingUniform(const LightingState& lighting, UniformBlock& uniform)
{
    const DirectionalLight& directional = lighting.directional;
    const AmbientLight& ambient = lighting.ambient;
    const float azimuthRadians = xm::DegreesToRadians(std::clamp(directional.azimuthDegrees, 0.0f, 360.0f));
    const float elevationRadians = xm::DegreesToRadians(std::clamp(directional.elevationDegrees, 0.0f, 90.0f));
    const WorldVec3 sunDir = WorldDirectionFromAzimuthElevation(azimuthRadians, elevationRadians);
    const float sunIntensity = std::max(0.0f, directional.intensity) * (directional.enabled ? 1.0f : 0.0f);
    const float ambientIntensity = std::max(0.0f, ambient.intensity);
    uniform.sunDir[0] = sunDir.x;
    uniform.sunDir[1] = sunDir.y;
    uniform.sunDir[2] = sunDir.z;
    uniform.sunDir[3] = 0.0f;
    uniform.sunColor[0] = std::max(0.0f, directional.r) * sunIntensity;
    uniform.sunColor[1] = std::max(0.0f, directional.g) * sunIntensity;
    uniform.sunColor[2] = std::max(0.0f, directional.b) * sunIntensity;
    uniform.sunColor[3] = 0.0f;
    uniform.ambientColor[0] = std::max(0.0f, ambient.r) * ambientIntensity;
    uniform.ambientColor[1] = std::max(0.0f, ambient.g) * ambientIntensity;
    uniform.ambientColor[2] = std::max(0.0f, ambient.b) * ambientIntensity;
    uniform.ambientColor[3] = 0.0f;
    uniform.numPointLights = static_cast<std::int32_t>(
        std::min<std::uint32_t>(lighting.numPointLights, kMaxDynamicPointLights));
    uniform.numSpotLights = static_cast<std::int32_t>(
        std::min<std::uint32_t>(lighting.numSpotLights, kMaxDynamicSpotLights));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(uniform.numPointLights); ++i)
    {
        const PointLight& point = lighting.pointLights[i];
        auto& out = uniform.pointLights[i];
        out.position[0] = point.position[0];
        out.position[1] = point.position[1];
        out.position[2] = point.position[2];
        out.position[3] = std::max(0.1f, point.radius);
        const float intensity = point.enabled ? std::max(0.0f, point.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, point.r);
        out.color[1] = std::max(0.0f, point.g);
        out.color[2] = std::max(0.0f, point.b);
        out.color[3] = intensity;
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(uniform.numSpotLights); ++i)
    {
        SpotLight spot = lighting.spotLights[i];
        spot.outerConeDegrees = std::clamp(spot.outerConeDegrees, 1.0f, 90.0f);
        spot.innerConeDegrees = std::clamp(spot.innerConeDegrees, 1.0f, spot.outerConeDegrees);
        const WorldVec3 spotDir = WorldForwardFromYawPitch(spot.rotation[1], spot.rotation[0]);
        auto& out = uniform.spotLights[i];
        out.position[0] = spot.position[0];
        out.position[1] = spot.position[1];
        out.position[2] = spot.position[2];
        out.position[3] = std::max(0.1f, spot.radius);
        out.direction[0] = spotDir.x;
        out.direction[1] = spotDir.y;
        out.direction[2] = spotDir.z;
        out.direction[3] = xm::Cos(xm::DegreesToRadians(spot.innerConeDegrees));
        const float intensity = spot.enabled ? std::max(0.0f, spot.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, spot.r) * intensity;
        out.color[1] = std::max(0.0f, spot.g) * intensity;
        out.color[2] = std::max(0.0f, spot.b) * intensity;
        out.color[3] = xm::Cos(xm::DegreesToRadians(spot.outerConeDegrees));
        out.direction[3] = std::max(out.direction[3], out.color[3]);
    }
}

Mat4 BuildStaticMeshModelMatrix(const StaticMeshRenderer::Instance& instance)
{
    return Multiply(
        Multiply(
            Multiply(
                Multiply(Scale(instance.scale[0], instance.scale[1], instance.scale[2]),
                    RotationX(instance.rotation[0])),
                RotationY(instance.rotation[1])),
            RotationZ(instance.rotation[2])),
        Translation(instance.position.x, instance.position.y, instance.position.z));
}

float AlphaModeCode(const std::string& alphaMode);
const char* AlphaFragmentPath(const std::string& alphaMode);

StaticMeshRenderer::MaterialDefaults MaterialDefaultsFromAsset(const MaterialAsset& material)
{
    StaticMeshRenderer::MaterialDefaults defaults{};
    defaults.baseColor[0] = material.baseColor[0];
    defaults.baseColor[1] = material.baseColor[1];
    defaults.baseColor[2] = material.baseColor[2];
    defaults.baseColor[3] = material.baseColor[3];
    defaults.metallic = material.metallic;
    defaults.roughness = material.roughness;
    defaults.normalStrength = material.normalStrength;
    defaults.aoStrength = material.aoStrength;
    defaults.emissive[0] = material.emissive[0] * material.emissive[3];
    defaults.emissive[1] = material.emissive[1] * material.emissive[3];
    defaults.emissive[2] = material.emissive[2] * material.emissive[3];
    defaults.alphaMode = material.alphaMode == MaterialAsset::AlphaMode::Mask ? "mask" :
        (material.alphaMode == MaterialAsset::AlphaMode::Blend ? "blend" : "opaque");
    defaults.alphaCutoff = material.alphaCutoff;
    defaults.unlit = material.shadingMode == MaterialAsset::ShadingMode::Unlit;
    defaults.uvTiling[0] = material.uvTiling[0];
    defaults.uvTiling[1] = material.uvTiling[1];
    defaults.uvOffset[0] = material.uvOffset[0];
    defaults.uvOffset[1] = material.uvOffset[1];
    return defaults;
}

void LogPinkFallbackOnce(std::uint32_t entityId,
                         std::uint32_t materialSlot,
                         const char* reason,
                         const std::string& requestedGuid)
{
    static std::unordered_set<std::string> logged;
    const std::string key = std::to_string(entityId) + ":" + std::to_string(materialSlot) + ":" +
        reason + ":" + requestedGuid;
    if (!logged.insert(key).second)
        return;
    Tracenf("[MATERIAL-SLOTS] resolved_to_pink entity=%u slot=%u reason=%s requestedGuid=%s",
        entityId,
        materialSlot,
        reason,
        requestedGuid.empty() ? "<empty>" : requestedGuid.c_str());
}

StaticMeshRenderer::MaterialDefaults ResolveMaterialSlotDefaults(
    const StaticMeshRenderer::Instance& instance,
    uint32_t materialSlot,
    const std::vector<StaticMeshRenderer::MaterialDefaults>& bakedDefaults,
    bool internalBakedMaterial = false)
{
    const StaticMeshRenderer::MaterialDefaults bakedFallback{};
    StaticMeshRenderer::MaterialDefaults defaults = bakedDefaults.empty()
        ? bakedFallback
        : (materialSlot < bakedDefaults.size() ? bakedDefaults[materialSlot] : bakedDefaults.front());
    if (internalBakedMaterial) return defaults;

    if (materialSlot >= instance.materialSlots.size() || instance.materialSlots[materialSlot].empty())
    {
        LogPinkFallbackOnce(instance.entityId, materialSlot, "empty_guid", {});
        if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
            return MaterialDefaultsFromAsset(*pink);
        return defaults;
    }

    const std::string& guidText = instance.materialSlots[materialSlot];
    const std::optional<Guid> guid = Guid::fromString(guidText);
    if (!guid)
    {
        LogPinkFallbackOnce(instance.entityId, materialSlot, "invalid_guid", guidText);
        if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
            return MaterialDefaultsFromAsset(*pink);
        return defaults;
    }

    if (MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(*guid))
        return MaterialDefaultsFromAsset(*material);

    LogPinkFallbackOnce(instance.entityId, materialSlot, "guid_not_found", guidText);
    if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
        return MaterialDefaultsFromAsset(*pink);
    return defaults;
}

void FillStaticMeshInstanceBlock(const Mat4& viewProjection,
    const StaticMeshRenderer::Instance& instance,
    uint32_t materialSlot,
    const std::vector<StaticMeshRenderer::MaterialDefaults>& materialDefaults,
    StaticMeshRenderer::InstanceBlock& out,
    bool internalBakedMaterial = false)
{
    out = {};
    out.model = BuildStaticMeshModelMatrix(instance);
    out.mvp = Multiply(out.model, viewProjection);
    out.tint[0] = instance.tint[0];
    out.tint[1] = instance.tint[1];
    out.tint[2] = instance.tint[2];
    out.tint[3] = instance.tint[3];

    const StaticMeshRenderer::MaterialDefaults defaults =
        ResolveMaterialSlotDefaults(instance, materialSlot, materialDefaults, internalBakedMaterial);
    std::memcpy(out.materialBaseColor, defaults.baseColor, sizeof(out.materialBaseColor));
    out.materialParams[0] = defaults.metallic;
    out.materialParams[1] = defaults.roughness;
    out.materialParams[2] = defaults.normalStrength;
    out.materialParams[3] = defaults.aoStrength;
    out.materialEmissive[0] = defaults.emissive[0];
    out.materialEmissive[1] = defaults.emissive[1];
    out.materialEmissive[2] = defaults.emissive[2];
    out.materialEmissive[3] = 1.0f;
    out.materialUv[0] = defaults.uvTiling[0];
    out.materialUv[1] = defaults.uvTiling[1];
    out.materialUv[2] = defaults.uvOffset[0];
    out.materialUv[3] = defaults.uvOffset[1];
    out.materialAlpha[0] = AlphaModeCode(defaults.alphaMode);
    out.materialAlpha[1] = std::clamp(defaults.alphaCutoff, 0.0f, 1.0f);
    out.materialAlpha[2] = instance.coverageMin;
    out.materialAlpha[3] = instance.coverageMax;

    for (const MeshSceneEntity::MaterialOverride& overrideSlot : instance.materialOverrides)
    {
        if (overrideSlot.slot != materialSlot || !overrideSlot.enabled)
            continue;
        std::memcpy(out.materialBaseColor, overrideSlot.baseColor, sizeof(out.materialBaseColor));
        out.materialParams[0] = overrideSlot.metallic;
        out.materialParams[1] = overrideSlot.roughness;
        out.materialParams[2] = overrideSlot.normalStrength;
        out.materialParams[3] = overrideSlot.aoStrength;
        out.materialEmissive[0] = overrideSlot.emissive[0];
        out.materialEmissive[1] = overrideSlot.emissive[1];
        out.materialEmissive[2] = overrideSlot.emissive[2];
        out.materialEmissive[3] = overrideSlot.emissiveIntensity;
        out.materialUv[0] = overrideSlot.uvTiling[0];
        out.materialUv[1] = overrideSlot.uvTiling[1];
        out.materialUv[2] = overrideSlot.uvOffset[0];
        out.materialUv[3] = overrideSlot.uvOffset[1];
        break;
    }
}

bool CopyDataSourceBytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source,
    size_t byteOffset, size_t byteLength, std::vector<uint8_t>& out);

bool CopyBufferViewBytes(const fastgltf::Asset& asset, size_t bufferViewIndex, std::vector<uint8_t>& out)
{
    if (bufferViewIndex >= asset.bufferViews.size())
        return false;
    const auto& view = asset.bufferViews[bufferViewIndex];
    if (view.bufferIndex >= asset.buffers.size())
        return false;
    return CopyDataSourceBytes(asset, asset.buffers[view.bufferIndex].data,
        view.byteOffset, view.byteLength, out);
}

bool CopyDataSourceBytes(const fastgltf::Asset& asset, const fastgltf::DataSource& source,
    size_t byteOffset, size_t byteLength, std::vector<uint8_t>& out)
{
    const std::byte* data = nullptr;
    size_t size = 0;
    if (const auto* bufferView = std::get_if<fastgltf::sources::BufferView>(&source))
        return CopyBufferViewBytes(asset, bufferView->bufferViewIndex, out);
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&source))
    {
        data = array->bytes.data();
        size = array->bytes.size();
    }
    else if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&source))
    {
        data = vector->bytes.data();
        size = vector->bytes.size();
    }
    else if (const auto* byteView = std::get_if<fastgltf::sources::ByteView>(&source))
    {
        data = byteView->bytes.data();
        size = byteView->bytes.size();
    }
    else
    {
        return false;
    }

    if (!data || byteOffset > size)
        return false;
    const size_t available = size - byteOffset;
    const size_t length = byteLength == std::numeric_limits<size_t>::max() ? available : byteLength;
    if (length > available)
        return false;
    const auto* begin = reinterpret_cast<const uint8_t*>(data + byteOffset);
    out.assign(begin, begin + length);
    return true;
}

std::optional<fastgltf::Asset> ParseGltf(client::asset::IAssetReader& assets,
    const std::string& modelPath,
    std::string* error,
    bool loadExternalImages = true)
{
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);
    auto gltfDirectory = std::filesystem::path(dir);
    if (!gltfDirectory.is_absolute())
        if (auto root = assets.RootPath()) gltfDirectory = *root / gltfDirectory;
    auto modelBytes = assets.ReadAll(modelPath);
    if (!modelBytes)
    {
        if (error)
            *error = "file not found";
        return std::nullopt;
    }
    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(modelBytes->data()), modelBytes->size());
    if (data.error() != fastgltf::Error::None)
    {
        if (error)
            *error = std::string("data buffer error: ") + std::string(fastgltf::getErrorMessage(data.error()));
        return std::nullopt;
    }
    fastgltf::Options options =
        fastgltf::Options::DecomposeNodeMatrices |
        fastgltf::Options::LoadExternalBuffers |
        fastgltf::Options::GenerateMeshIndices;
    if (loadExternalImages)
        options |= fastgltf::Options::LoadExternalImages;

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), gltfDirectory,
        options);
    if (assetResult.error() != fastgltf::Error::None)
    {
        if (error)
            *error = std::string("parse error: ") + std::string(fastgltf::getErrorMessage(assetResult.error()));
        return std::nullopt;
    }
    return std::move(assetResult.get());
}

bool AssetLooksSkinned(const fastgltf::Asset& asset)
{
    if (!asset.skins.empty())
        return true;
    for (const auto& node : asset.nodes)
    {
        if (node.skinIndex.has_value())
            return true;
    }
    for (const auto& mesh : asset.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            if (primitive.findAttribute("JOINTS_0") != primitive.attributes.end() ||
                primitive.findAttribute("WEIGHTS_0") != primitive.attributes.end())
                return true;
        }
    }
    return false;
}

StaticMeshRenderer::MaterialDefaults ReadMaterialDefaults(const fastgltf::Material& material)
{
    StaticMeshRenderer::MaterialDefaults defaults{};
    defaults.baseColor[0] = material.pbrData.baseColorFactor.x();
    defaults.baseColor[1] = material.pbrData.baseColorFactor.y();
    defaults.baseColor[2] = material.pbrData.baseColorFactor.z();
    defaults.baseColor[3] = material.pbrData.baseColorFactor.w();
    defaults.metallic = material.pbrData.metallicFactor;
    defaults.roughness = material.pbrData.roughnessFactor;
    if (material.normalTexture.has_value())
        defaults.normalStrength = material.normalTexture->scale;
    if (material.occlusionTexture.has_value())
        defaults.aoStrength = material.occlusionTexture->strength;
    defaults.emissive[0] = material.emissiveFactor.x();
    defaults.emissive[1] = material.emissiveFactor.y();
    defaults.emissive[2] = material.emissiveFactor.z();
    defaults.alphaMode = material.alphaMode == fastgltf::AlphaMode::Mask ? "mask" :
        (material.alphaMode == fastgltf::AlphaMode::Blend ? "blend" : "opaque");
    defaults.alphaCutoff = material.alphaCutoff;
    return defaults;
}

StaticMeshRenderer::MaterialDefaults MaterialDefaultsFromSource(const GltfMaterialSource& source)
{
    StaticMeshRenderer::MaterialDefaults defaults{};
    defaults.baseColor[0] = source.baseColor[0];
    defaults.baseColor[1] = source.baseColor[1];
    defaults.baseColor[2] = source.baseColor[2];
    defaults.baseColor[3] = source.baseColor[3];
    defaults.metallic = source.metallic;
    defaults.roughness = source.roughness;
    defaults.normalStrength = source.normalStrength;
    defaults.aoStrength = source.aoStrength;
    defaults.emissive[0] = source.emissive[0] * source.emissive[3];
    defaults.emissive[1] = source.emissive[1] * source.emissive[3];
    defaults.emissive[2] = source.emissive[2] * source.emissive[3];
    defaults.alphaMode = source.alphaMode.empty() ? "opaque" : source.alphaMode;
    defaults.alphaCutoff = source.alphaCutoff;
    return defaults;
}

std::optional<std::filesystem::path> TexturePathFromInfo(const fastgltf::Asset& asset,
                                                         const fastgltf::TextureInfo& info,
                                                         const std::filesystem::path& modelDir)
{
    if (info.textureIndex >= asset.textures.size())
        return std::nullopt;
    const fastgltf::Texture& texture = asset.textures[info.textureIndex];
    std::optional<std::size_t> imageIndex = texture.imageIndex;
    if (!imageIndex)
        imageIndex = texture.ddsImageIndex;
    if (!imageIndex)
        imageIndex = texture.basisuImageIndex;
    if (!imageIndex || *imageIndex >= asset.images.size())
        return std::nullopt;

    const auto& image = asset.images[*imageIndex];
    const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data);
    if (!uri || !uri->uri.isLocalPath() || uri->uri.isDataUri())
        return std::nullopt;

    std::filesystem::path path = uri->uri.fspath();
    if (path.is_relative())
        path = modelDir / path;
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path) : normalized;
}

std::string AlphaModeString(fastgltf::AlphaMode mode)
{
    switch (mode)
    {
    case fastgltf::AlphaMode::Mask: return "mask";
    case fastgltf::AlphaMode::Blend: return "blend";
    default: return "opaque";
    }
}

const char* MaterialAlphaModeName(MaterialAsset::AlphaMode mode)
{
    switch (mode)
    {
    case MaterialAsset::AlphaMode::Mask: return "MASK";
    case MaterialAsset::AlphaMode::Blend: return "BLEND";
    default: return "OPAQUE";
    }
}

float AlphaModeCode(const std::string& alphaMode)
{
    if (alphaMode == "mask" || alphaMode == "MASK")
        return 1.0f;
    if (alphaMode == "blend" || alphaMode == "BLEND")
        return 2.0f;
    return 0.0f;
}

const char* AlphaFragmentPath(const std::string& alphaMode)
{
    if (alphaMode == "mask" || alphaMode == "MASK")
        return "discard";
    if (alphaMode == "blend" || alphaMode == "BLEND")
        return "blend";
    return "none";
}

// Whether the material in an instance's slot is alpha-blended (only a material asset sets that).
bool IsBlendMaterialSlot(const StaticMeshRenderer::Instance& instance, std::uint32_t materialSlot)
{
    if (materialSlot >= instance.materialSlots.size())
        return false;
    const std::optional<Guid> guid = Guid::fromString(instance.materialSlots[materialSlot]);
    if (!guid)
        return false;
    const MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(*guid);
    return material && material->alphaMode == MaterialAsset::AlphaMode::Blend;
}

std::string SanitizedStem(std::string value)
{
    for (char& c : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    while (!value.empty() && value.back() == '_')
        value.pop_back();
    return value.empty() ? "asset" : value;
}

std::string LowercaseExtension(const std::string& modelPath)
{
    std::string ext = std::filesystem::path(modelPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

GltfMaterialSource BuildMaterialSource(const fastgltf::Asset& asset,
                                       const fastgltf::Material* material,
                                       std::size_t index,
                                       const std::filesystem::path& modelDir)
{
    GltfMaterialSource source{};
    source.name = material && !material->name.empty()
        ? std::string(material->name)
        : ("material_" + std::to_string(index));
    if (!material)
        return source;

    source.baseColor[0] = material->pbrData.baseColorFactor.x();
    source.baseColor[1] = material->pbrData.baseColorFactor.y();
    source.baseColor[2] = material->pbrData.baseColorFactor.z();
    source.baseColor[3] = material->pbrData.baseColorFactor.w();
    source.metallic = material->pbrData.metallicFactor;
    source.roughness = material->pbrData.roughnessFactor;
    source.emissive[0] = material->emissiveFactor.x();
    source.emissive[1] = material->emissiveFactor.y();
    source.emissive[2] = material->emissiveFactor.z();
    source.emissive[3] = material->emissiveStrength;
    source.alphaMode = AlphaModeString(material->alphaMode);
    source.alphaCutoff = material->alphaCutoff;
    if (material->normalTexture.has_value())
    {
        source.normalStrength = material->normalTexture->scale;
        if (const auto path = TexturePathFromInfo(asset, *material->normalTexture, modelDir))
            source.normalTexturePath = *path;
    }
    if (material->occlusionTexture.has_value())
    {
        source.aoStrength = material->occlusionTexture->strength;
        if (const auto path = TexturePathFromInfo(asset, *material->occlusionTexture, modelDir))
            source.aoTexturePath = *path;
    }
    if (material->pbrData.baseColorTexture.has_value())
    {
        if (const auto path = TexturePathFromInfo(asset, *material->pbrData.baseColorTexture, modelDir))
            source.baseColorTexturePath = *path;
    }
    if (material->pbrData.metallicRoughnessTexture.has_value())
    {
        if (const auto path = TexturePathFromInfo(asset, *material->pbrData.metallicRoughnessTexture, modelDir))
            source.metallicRoughnessTexturePath = *path;
    }
    if (material->emissiveTexture.has_value())
    {
        if (const auto path = TexturePathFromInfo(asset, *material->emissiveTexture, modelDir))
            source.emissiveTexturePath = *path;
    }
    return source;
}

std::vector<Guid> GenerateMaterialAssetsForGltf(const fastgltf::Asset& asset, const std::string& modelPath)
{
    std::vector<Guid> defaultMaterials;
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return defaultMaterials;

    std::filesystem::path modelFsPath(modelPath);
    if (modelFsPath.is_relative())
    {
        const std::string generic = modelFsPath.generic_string();
        modelFsPath = (generic == "Assets" || generic.rfind("Assets/", 0) == 0)
            ? (projects.ProjectRoot() / modelFsPath)
            : (projects.AssetRootPath() / modelFsPath);
    }
    std::error_code modelPathEc;
    modelFsPath = std::filesystem::weakly_canonical(modelFsPath, modelPathEc);
    const std::filesystem::path modelDir = modelFsPath.has_parent_path()
        ? modelFsPath.parent_path()
        : std::filesystem::current_path();
    const std::string meshName = SanitizedStem(modelFsPath.stem().string());
    // The model's generated materials sit next to it, in <model>_materials/ (no per-type folder).
    const std::filesystem::path materialFolder = modelDir / (meshName + "_materials");

    auto& manager = MaterialAssetManager::Instance();
    MaterialAssetManager::ImportSummary summary{};
    const std::size_t materialCount = asset.materials.empty() ? 1u : asset.materials.size();
    // A model that has its materials already (its .meta's default materials, one per glTF material,
    // all still there: the ones it was imported or saved with, or chosen for it) keeps them. Made
    // again from the file on every load, any that differed from the saved one (a tree's, by its
    // emissive strength) was saved again as <name>_v2 and replaced the model's own, a chosen one too.
    {
        const std::vector<Guid> existing = AssetDatabase::Instance().loadDefaultMaterials(modelFsPath);
        const bool allThere = existing.size() == materialCount &&
            std::all_of(existing.begin(), existing.end(), [](const Guid& guid) {
                const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(guid);
                std::error_code exists;
                return path && std::filesystem::exists(*path, exists);
            });
        if (allThere)
            return existing;
    }
    summary.materials = static_cast<std::uint32_t>(materialCount);
    defaultMaterials.reserve(materialCount);
    for (std::size_t i = 0; i < materialCount; ++i)
    {
        const fastgltf::Material* material = asset.materials.empty() ? nullptr : &asset.materials[i];
        GltfMaterialSource source = BuildMaterialSource(asset, material, i, modelDir);
        defaultMaterials.push_back(manager.createFromGltfMaterial(source, materialFolder, source.name, &summary));
    }

    if (!defaultMaterials.empty())
    {
        AssetDatabase::Instance().writeDefaultMaterials(modelFsPath, defaultMaterials);
        Tracenf("[MATERIAL-SLOTS] defaultMaterials_recorded model=%s count=%zu",
            modelFsPath.filename().generic_string().c_str(),
            defaultMaterials.size());
    }

    Tracenf("[MATERIAL-ASSET] gltf_import_summary mesh=%s materials=%u generated=%u reused=%u conflicts=%u",
        modelFsPath.filename().generic_string().c_str(),
        summary.materials,
        summary.generated,
        summary.reused,
        summary.conflicts);
    return defaultMaterials;
}

RgbaImage CreateFallbackWhiteImage(const std::string& modelPath)
{
    RgbaImage image{};
    image.name = modelPath + "#fallback-white";
    image.width = 4;
    image.height = 4;
    image.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    image.pixels.assign(static_cast<size_t>(image.width) * image.height * 4u, 0xff);
    return image;
}

RgbaImage CreateFallbackNormalImage(const std::string& modelPath)
{
    RgbaImage image{};
    image.name = modelPath + "#fallback-normal";
    image.width = 4;
    image.height = 4;
    image.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4u);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
    {
        image.pixels[i + 0] = 128;
        image.pixels[i + 1] = 128;
        image.pixels[i + 2] = 255;
        image.pixels[i + 3] = 255;
    }
    return image;
}

RgbaImage CreateFallbackOrmImage(const std::string& modelPath)
{
    RgbaImage image{};
    image.name = modelPath + "#fallback-orm";
    image.width = 4;
    image.height = 4;
    image.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4u);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
    {
        image.pixels[i + 0] = 255; // AO
        image.pixels[i + 1] = 255; // Roughness
        image.pixels[i + 2] = 255; // Metallic factor texture
        image.pixels[i + 3] = 255;
    }
    return image;
}

bool DecodeGltfTexture(const fastgltf::Asset& asset,
    size_t textureIndex,
    ixrhi::IXRHIFormat format,
    const char* label,
    RgbaImage& out)
{
    if (textureIndex >= asset.textures.size())
        return false;
    const auto& texture = asset.textures[textureIndex];
    if (!texture.imageIndex.has_value() || texture.imageIndex.value() >= asset.images.size())
        return false;
    const auto& image = asset.images[texture.imageIndex.value()];
    std::vector<uint8_t> encoded;
    if (!CopyDataSourceBytes(asset, image.data, 0, std::numeric_limits<size_t>::max(), encoded) ||
        encoded.empty())
    {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    if (client::asset::IsExr(encoded))
    {
        std::string error;
        auto exr = client::asset::DecodeExr(encoded, error);
        if (!exr) { LogFormat("[EXR] %s", error.c_str()); return false; }
        out.name = std::string(image.name.empty() ? label : image.name);
        out.width = static_cast<std::uint32_t>(exr->width);
        out.height = static_cast<std::uint32_t>(exr->height);
        out.format = ixrhi::IXRHIFormat::R16G16B16A16Float;
        out.pixels = client::asset::ExrHalfPixels(*exr);
        return true;
    }
    stbi_uc* decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }

    out.name = std::string(image.name.empty() ? label : image.name);
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.format = format;
    out.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    stbi_image_free(decoded);
    LogFormat("[STATIC-MESH] decoded %s image='%s' (%ux%u, source channels=%d)",
        label, out.name.c_str(), out.width, out.height, channels);
    return true;
}

bool DecodeTextureFile(const std::filesystem::path& path,
    ixrhi::IXRHIFormat format,
    const char* label,
    RgbaImage& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encoded.empty())
        return false;

    if (client::asset::IsExr(encoded) || client::asset::IsExrPath(path))
    {
        std::string error;
        auto exr = client::asset::DecodeExr(encoded, error);
        if (!exr) { LogFormat("[EXR] %s", error.c_str()); return false; }
        out.name = path.filename().generic_string();
        out.width = static_cast<std::uint32_t>(exr->width);
        out.height = static_cast<std::uint32_t>(exr->height);
        out.format = ixrhi::IXRHIFormat::R16G16B16A16Float;
        out.pixels = client::asset::ExrHalfPixels(*exr);
        return true;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }

    out.name = path.filename().generic_string();
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.format = format;
    out.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    stbi_image_free(decoded);
    LogFormat("[STATIC-MESH] decoded material %s image='%s' (%ux%u, source channels=%d)",
        label, out.name.c_str(), out.width, out.height, channels);
    return true;
}

bool LoadGltfMaterialTextures(client::asset::IAssetReader& assets,
    const std::string& modelPath,
    RgbaImage& diffuse,
    RgbaImage& normal,
    RgbaImage& orm)
{
    std::string error;
    auto parsed = ParseGltf(assets, modelPath, &error);
    if (!parsed)
        return false;
    const fastgltf::Asset& asset = *parsed;
    bool loadedAny = false;
    for (const auto& material : asset.materials)
    {
        if (material.pbrData.baseColorTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.pbrData.baseColorTexture->textureIndex,
                ixrhi::IXRHIFormat::R8G8B8A8Srgb, "baseColorTexture", diffuse);
        if (material.normalTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.normalTexture->textureIndex,
                ixrhi::IXRHIFormat::R8G8B8A8Unorm, "normalTexture", normal);
        if (material.pbrData.metallicRoughnessTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.pbrData.metallicRoughnessTexture->textureIndex,
                ixrhi::IXRHIFormat::R8G8B8A8Unorm, "metallicRoughnessTexture", orm);
        else if (material.occlusionTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.occlusionTexture->textureIndex,
                ixrhi::IXRHIFormat::R8G8B8A8Unorm, "occlusionTexture", orm);
        if (loadedAny)
            break;
    }
    return loadedAny;
}

// The material texture cache key's role (EnsureMaterialTexture): the decode differs per role.
std::uint8_t MaterialTextureRoleIndex(const char* role)
{
    if (role == nullptr)
        return 0;
    if (std::strcmp(role, "baseColor") == 0)
        return 1;
    if (std::strcmp(role, "normal") == 0)
        return 2;
    if (std::strcmp(role, "metallicRoughness") == 0)
        return 3;
    return 4;
}

} // namespace

struct StaticMeshRenderer::MaterialTextureLoad
{
    ixjobs::Counter decoded;  // the decode job
    std::filesystem::path path;
    ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    std::string role;
    RgbaImage image;
    bool ok = false;
    ~MaterialTextureLoad() { ixjobs::JobSystem::Instance().Wait(decoded, /*runBackground=*/true); }
};

struct StaticMeshRenderer::PendingMaterialImport
{
    fastgltf::Asset asset;
};

struct StaticMeshRenderer::TreeImpostorState
{
    tree_tool::TreeImpostorData data;
    std::unique_ptr<StaticMeshRenderer> renderer;
    std::uint64_t checkedRevision = 0;
    std::vector<std::filesystem::file_time_type> sourceTimes;
    std::filesystem::file_time_type metadataTime{};
    std::chrono::steady_clock::time_point checkedFiles{};
    bool filesValid = true;
    bool valid = true;
    bool partitioning = false;
    InstanceList nearInstances, farInstances;
    std::vector<Instance> fading, billboards;
    std::vector<PreparedInstance> billboardPrepared;
};

StaticMeshRenderer::StaticMeshRenderer() = default;

StaticMeshRenderer::~StaticMeshRenderer()
{
    Destroy();
}

bool StaticMeshRenderer::DetectSkinnedGltf(client::asset::IAssetReader& assets,
    const std::string& modelPath,
    bool& outSkinned,
    std::string* error)
{
    outSkinned = false;
    auto parsed = ParseGltf(assets, modelPath, error);
    if (!parsed)
        return false;
    outSkinned = AssetLooksSkinned(*parsed);
    return true;
}

bool StaticMeshRenderer::Create(ixrhi::IXRHIDevice& rhi,
    client::asset::IAssetReader& assets,
    const std::string& modelPath)
{
    Destroy();
    return LoadCpu(assets, modelPath) && FinishGpu(rhi);
}

bool StaticMeshRenderer::LoadCpu(client::asset::IAssetReader& assets, const std::string& modelPath)
{
    m_assets = &assets;
    m_modelPath = modelPath;
    m_status = LoadStatus::Failed;
    m_pendingMaterialImport.reset();
    auto logLoadFailure = [&]() {
        LogFormat("[MESH] Create: loaded=0 buffers=0 textures=0 descriptors=0 pipeline=0 verts=%zu indices=%zu drawcalls=%zu model=%s",
            m_vertices.size(),
            m_indices.size(),
            m_draws.size(),
            modelPath.c_str());
    };

    const bool builtinPrimitive = modelPath.rfind("builtin://primitive/", 0) == 0;
    const std::string ext = LowercaseExtension(modelPath);
    if (builtinPrimitive)
    {
        if (!LoadBuiltinPrimitiveMesh(modelPath))
        {
            logLoadFailure();
            return false;
        }
    }
    else if (ext == ".fbx")
    {
        if (!LoadStaticFbxMesh(modelPath))
        {
            logLoadFailure();
            return false;
        }
    }
    else
    {
        bool isSkinned = false;
        std::string error;
        if (!DetectSkinnedGltf(assets, modelPath, isSkinned, &error))
        {
            LogFormat("[STATIC-MESH] inspect failed: %s reason=%s", modelPath.c_str(), error.c_str());
            logLoadFailure();
            return false;
        }
        if (isSkinned)
        {
            m_status = LoadStatus::UnsupportedSkinned;
            LogFormat("[STATIC-MESH] skinned glTF detected, static renderer will not load it: %s", modelPath.c_str());
            logLoadFailure();
            return false;
        }

        if (!LoadStaticGltfMesh(modelPath))
        {
            logLoadFailure();
            return false;
        }
    }
    BuildLods();
    DecodeTextures(modelPath);
    if (!builtinPrimitive) LoadTreeImpostorCpu();
    m_status = LoadStatus::NotLoaded;  // on the CPU; FinishGpu makes it LoadedStatic
    return true;
}

void StaticMeshRenderer::LoadTreeImpostorCpu()
{
    m_treeImpostor.reset();
    if (!m_isTreeImpostor)
    {
        std::string impostorError;
        if (auto data = tree_tool::LoadTreeImpostor(*m_assets, m_modelPath, impostorError))
        {
            auto state = std::make_unique<TreeImpostorState>();
            state->data = std::move(*data);
            if (auto root = m_assets->RootPath())
            {
                std::error_code metadataError;
                state->metadataTime = std::filesystem::last_write_time(*root / (m_modelPath + ".impostor.json"), metadataError);
                state->filesValid &= !metadataError;
                for (const auto& source : state->data.sourcePaths)
                {
                    std::error_code error;
                    state->sourceTimes.push_back(std::filesystem::last_write_time(*root / source, error));
                    state->filesValid &= !error;
                }
            }
            state->renderer = std::make_unique<StaticMeshRenderer>();
            state->renderer->m_isTreeImpostor = true; // Never recursively load another impostor.
            if (state->renderer->LoadCpu(*m_assets, state->data.billboardPath) &&
                state->renderer->VertexCount() == 4 && state->renderer->IndexCount() == 6)
                m_treeImpostor = std::move(state);
        }
        else if (!impostorError.empty()) LogFormat("[TREE-IMPOSTOR] fallback model=%s reason=%s", m_modelPath.c_str(), impostorError.c_str());
    }
}

void StaticMeshRenderer::ReloadTreeImpostor()
{
    if (!m_rhi || !m_assets || !IsLoaded() || m_isTreeImpostor) return;
    LoadTreeImpostorCpu();
    if (m_treeImpostor)
    {
        m_treeImpostor->renderer->SetTargetPass(m_targetPass);
        if (!m_treeImpostor->renderer->FinishGpu(*m_rhi)) m_treeImpostor.reset();
    }
    LogFormat("[TREE-IMPOSTOR] reloaded model=%s active=%u", m_modelPath.c_str(), m_treeImpostor ? 1u : 0u);
}

void StaticMeshRenderer::BuildLods()
{
    m_lodIndices.clear();
    for (std::vector<MeshDraw>& draws : m_shadowLodDraws)
        draws.clear();
    for (std::vector<MeshDraw>& draws : m_viewLodDraws)
        draws.clear();
    m_modelRadius = 0.0f;
    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? m_boundsMax[0] : m_boundsMin[0];
        const float y = (corner & 2) ? m_boundsMax[1] : m_boundsMin[1];
        const float z = (corner & 4) ? m_boundsMax[2] : m_boundsMin[2];
        m_modelRadius = std::max(m_modelRadius, std::sqrt(x * x + y * y + z * z));
    }
    m_shadowCardVertices.clear();
    m_shadowCardDraws.clear();
    if (m_vertices.empty() || m_indices.empty())
        return;
    const auto masked = [&](const MeshDraw& draw) {
        return draw.materialSlot < m_materialDefaults.size() &&
            AlphaModeCode(m_materialDefaults[draw.materialSlot].alphaMode) >= 1.0f;
    };
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> welded(m_indices.size());
    meshopt_generateShadowIndexBuffer(welded.data(), m_indices.data(), m_indices.size(),
        &m_vertices.front().position[0], m_vertices.size(), sizeof(float) * 3u, sizeof(Vertex));

    // Opaque submeshes, simplified (a model of a few hundred opaque triangles keeps its own).
    std::size_t opaqueIndices = 0;
    for (const MeshDraw& draw : m_draws)
        opaqueIndices += masked(draw) ? 0u : draw.indexCount;
    std::array<std::size_t, kLodLevels> levelIndices{};
    std::array<std::size_t, kLodLevels> viewLevelIndices{};
    // Each opaque draw's levels, from the given indices (welded for the shadow, as they are for the view).
    const auto simplifyLevels = [&](const std::vector<std::uint32_t>& sourceIndices,
                                    std::array<std::vector<MeshDraw>, kLodLevels>& out,
                                    std::array<std::size_t, kLodLevels>& outIndices) {
        for (std::vector<MeshDraw>& draws : out)
            draws.assign(m_draws.size(), MeshDraw{});
        std::vector<std::uint32_t> source;
        std::vector<std::uint32_t> simplified;
        for (std::size_t drawIndex = 0; drawIndex < m_draws.size(); ++drawIndex)
        {
            const MeshDraw& draw = m_draws[drawIndex];
            if (draw.indexCount == 0 || masked(draw))
                continue;
            source.assign(sourceIndices.begin() + draw.firstIndex, sourceIndices.begin() + draw.firstIndex + draw.indexCount);
            MeshDraw kept{};  // the coarsest level kept so far (indexCount 0: the draw's own)
            float keptError = 0.0f;  // how far off the surface it is
            for (std::size_t level = 0; level < kLodLevels; ++level)
            {
                simplified.resize(source.size());
                float error = 0.0f;
                const std::size_t count = meshopt_simplify(simplified.data(), source.data(), source.size(),
                    &m_vertices.front().position[0], m_vertices.size(), sizeof(Vertex), 0,
                    std::max(kLodErrors[level] - keptError, 0.0f),
                    meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &error);
                if (count >= 3u && count * 4u <= source.size() * 3u)
                {
                    kept.firstIndex = static_cast<std::uint32_t>(m_lodIndices.size());
                    kept.indexCount = static_cast<std::uint32_t>(count);
                    kept.materialSlot = draw.materialSlot;
                    kept.vertexCount = draw.vertexCount;
                    m_lodIndices.insert(m_lodIndices.end(), simplified.begin(), simplified.begin() + count);
                    source.assign(simplified.begin(), simplified.begin() + count);
                    keptError += error;
                }
                out[level][drawIndex] = kept;
                outIndices[level] += kept.indexCount != 0 ? kept.indexCount : draw.indexCount;
            }
        }
    };
    if (opaqueIndices >= 3u * 256u)
    {
        simplifyLevels(welded, m_shadowLodDraws, levelIndices);
        simplifyLevels(m_indices, m_viewLodDraws, viewLevelIndices);
    }

    // Alpha-masked submeshes of many small separate pieces (leaf cards). A simplifier cannot merge
    // them; at the coarsest level a share of them is drawn instead, each grown about its centre to
    // cover the area of those left out (a crown's shadow keeps its density; kShadowCardCover of it,
    // as the grown pieces overlap each other less than the many small ones did).
    constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    constexpr float kFar = std::numeric_limits<float>::max();
    std::size_t cardIndicesBefore = 0;
    std::size_t cardIndicesAfter = 0;
    std::vector<std::uint32_t> parent;
    std::vector<std::uint32_t> pieceOfRoot;
    std::vector<std::uint32_t> cardVertexOf;
    for (std::size_t drawIndex = 0; drawIndex < m_draws.size(); ++drawIndex)
    {
        const MeshDraw& draw = m_draws[drawIndex];
        const std::uint32_t triangles = draw.indexCount / 3u;
        if (!masked(draw) || triangles < 256u)
            continue;
        if (parent.empty())
        {
            parent.resize(m_vertices.size());
            pieceOfRoot.assign(m_vertices.size(), kNone);
            cardVertexOf.assign(m_vertices.size(), kNone);
        }
        // The pieces: triangles joined through shared (welded) corners.
        const std::uint32_t* corners = welded.data() + draw.firstIndex;
        for (std::uint32_t i = 0; i < draw.indexCount; ++i)
            parent[corners[i]] = corners[i];
        const auto root = [&](std::uint32_t v) {
            while (parent[v] != v)
            {
                parent[v] = parent[parent[v]];
                v = parent[v];
            }
            return v;
        };
        for (std::uint32_t t = 0; t < triangles; ++t)
        {
            const std::uint32_t a = root(corners[t * 3u]);
            parent[root(corners[t * 3u + 1u])] = a;
            parent[root(corners[t * 3u + 2u])] = a;
        }
        std::vector<std::uint32_t> pieceOf(triangles);
        std::vector<std::uint32_t> pieceTriangles;
        std::vector<std::uint32_t> roots;
        for (std::uint32_t t = 0; t < triangles; ++t)
        {
            const std::uint32_t r = root(corners[t * 3u]);
            if (pieceOfRoot[r] == kNone)
            {
                pieceOfRoot[r] = static_cast<std::uint32_t>(pieceTriangles.size());
                pieceTriangles.push_back(0);
                roots.push_back(r);
            }
            pieceOf[t] = pieceOfRoot[r];
            ++pieceTriangles[pieceOf[t]];
        }
        for (const std::uint32_t r : roots)
            pieceOfRoot[r] = kNone;
        const std::uint32_t pieces = static_cast<std::uint32_t>(pieceTriangles.size());
        if (pieces < 64u || *std::max_element(pieceTriangles.begin(), pieceTriangles.end()) > 16u)
            continue;  // not cards

        // The kept pieces: an evenly mixed share (by a hash of their number, the same every load).
        const std::uint32_t keep = std::max(1u, static_cast<std::uint32_t>(std::lround(pieces * kShadowCardKeep)));
        std::vector<std::uint32_t> order(pieces);
        for (std::uint32_t p = 0; p < pieces; ++p)
            order[p] = p;
        std::nth_element(order.begin(), order.begin() + keep, order.end(),
            [](std::uint32_t a, std::uint32_t b) { return a * 0x9E3779B1u < b * 0x9E3779B1u; });
        std::vector<std::uint8_t> kept(pieces, 0);
        for (std::uint32_t k = 0; k < keep; ++k)
            kept[order[k]] = 1;
        const float grow = std::sqrt(kShadowCardCover * static_cast<float>(pieces) / static_cast<float>(keep));
        // Each kept piece's centre: the middle of its bounds.
        std::vector<std::array<float, 6>> bounds(pieces, {kFar, kFar, kFar, -kFar, -kFar, -kFar});
        for (std::uint32_t t = 0; t < triangles; ++t)
        {
            if (!kept[pieceOf[t]])
                continue;
            std::array<float, 6>& box = bounds[pieceOf[t]];
            for (std::uint32_t k = 0; k < 3u; ++k)
            {
                const float* p = m_vertices[m_indices[draw.firstIndex + t * 3u + k]].position;
                for (int axis = 0; axis < 3; ++axis)
                {
                    box[axis] = std::min(box[axis], p[axis]);
                    box[axis + 3] = std::max(box[axis + 3], p[axis]);
                }
            }
        }
        // Their triangles, on copies of their own vertices (uvs kept for the cut-out) moved out.
        MeshDraw cards{};
        cards.firstIndex = static_cast<std::uint32_t>(m_lodIndices.size());
        cards.materialSlot = draw.materialSlot;
        const std::size_t firstCardVertex = m_shadowCardVertices.size();
        for (std::uint32_t t = 0; t < triangles; ++t)
        {
            if (!kept[pieceOf[t]])
                continue;
            const std::array<float, 6>& box = bounds[pieceOf[t]];
            for (std::uint32_t k = 0; k < 3u; ++k)
            {
                const std::uint32_t source = m_indices[draw.firstIndex + t * 3u + k];
                if (cardVertexOf[source] == kNone)
                {
                    Vertex vertex = m_vertices[source];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float centre = 0.5f * (box[axis] + box[axis + 3]);
                        vertex.position[axis] = centre + (vertex.position[axis] - centre) * grow;
                    }
                    cardVertexOf[source] = static_cast<std::uint32_t>(m_shadowCardVertices.size());
                    m_shadowCardVertices.push_back(vertex);
                }
                m_lodIndices.push_back(cardVertexOf[source]);
            }
        }
        for (std::uint32_t i = 0; i < draw.indexCount; ++i)
            cardVertexOf[m_indices[draw.firstIndex + i]] = kNone;
        cards.indexCount = static_cast<std::uint32_t>(m_lodIndices.size() - cards.firstIndex);
        cards.vertexCount = static_cast<std::uint32_t>(m_shadowCardVertices.size() - firstCardVertex);
        if (m_shadowCardDraws.empty())
            m_shadowCardDraws.assign(m_draws.size(), MeshDraw{});
        m_shadowCardDraws[drawIndex] = cards;
        cardIndicesBefore += draw.indexCount;
        cardIndicesAfter += cards.indexCount;
    }

    if (m_lodIndices.empty())
    {
        for (std::vector<MeshDraw>& draws : m_shadowLodDraws)
            draws.clear();
        for (std::vector<MeshDraw>& draws : m_viewLodDraws)
            draws.clear();
        return;
    }
    LogFormat("[STATIC-MESH] lods: %s opaque_tris=%zu shadow=%zu/%zu/%zu view=%zu/%zu/%zu cards_tris=%zu->%zu (%.2f ms)",
        m_modelPath.c_str(), opaqueIndices / 3u, levelIndices[0] / 3u, levelIndices[1] / 3u, levelIndices[2] / 3u,
        viewLevelIndices[0] / 3u, viewLevelIndices[1] / 3u, viewLevelIndices[2] / 3u,
        cardIndicesBefore / 3u, cardIndicesAfter / 3u,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

bool StaticMeshRenderer::FinishGpu(ixrhi::IXRHIDevice& rhi, bool deferUploads)
{
    m_rhi = &rhi;
    m_deferUploads = deferUploads;
    m_pendingUploads = deferUploads ? std::make_unique<PendingUploads>() : nullptr;
    if (m_pendingMaterialImport)
    {
        GenerateMaterialAssetsForGltf(m_pendingMaterialImport->asset, m_modelPath);
        m_pendingMaterialImport.reset();
    }
    const std::string& modelPath = m_modelPath;
    const bool loaded = !m_vertices.empty() && !m_indices.empty();
    bool buffers = false;
    bool textures = false;
    bool descriptors = false;
    bool pipeline = false;
    m_status = LoadStatus::Failed;
    auto logCreateState = [&]() {
        LogFormat("[MESH] Create: loaded=%d buffers=%d textures=%d descriptors=%d pipeline=%d verts=%zu indices=%zu drawcalls=%zu texture=%s bbox_min=(%.3f,%.3f,%.3f) bbox_max=(%.3f,%.3f,%.3f)",
            loaded ? 1 : 0,
            buffers ? 1 : 0,
            textures ? 1 : 0,
            descriptors ? 1 : 0,
            pipeline ? 1 : 0,
            m_vertices.size(),
            m_indices.size(),
            m_draws.size(),
            m_texture.name.empty() ? "<none>" : m_texture.name.c_str(),
            m_boundsMin[0], m_boundsMin[1], m_boundsMin[2],
            m_boundsMax[0], m_boundsMax[1], m_boundsMax[2]);
    };
    if (!loaded)
    {
        logCreateState();
        return false;
    }
    if (!CreateBuffers(rhi))
    {
        logCreateState();
        return false;
    }
    buffers = HasVertexBuffer() && HasIndexBuffer();
    if (!UploadDecodedTextures(rhi))
    {
        logCreateState();
        return false;
    }
    textures = HasTexture();
    if (!CreateBindGroup(rhi))
    {
        logCreateState();
        return false;
    }
    descriptors = HasDescriptors();
    if (!CreatePipeline(rhi))
    {
        logCreateState();
        return false;
    }
    pipeline = HasPipeline();

    if (m_treeImpostor)
    {
        m_treeImpostor->renderer->SetTargetPass(m_targetPass);
        if (!m_treeImpostor->renderer->FinishGpu(rhi, deferUploads)) m_treeImpostor.reset();
        else LogFormat("[TREE-IMPOSTOR] ready model=%s views=%d start=%.1f transition=%.1f",
            modelPath.c_str(), m_treeImpostor->data.azimuths * 3, m_treeImpostor->data.distance, m_treeImpostor->data.transition);
    }
    m_status = LoadStatus::LoadedStatic;
    logCreateState();
    LogFormat("[STATIC-MESH] loaded: %s verts=%zu indices=%zu draws=%zu",
        modelPath.c_str(), m_vertices.size(), m_indices.size(), m_draws.size());
    return true;
}

bool StaticMeshRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || m_status != LoadStatus::LoadedStatic)
        return false;
    m_rhi = &rhi;
    DestroyPipeline();
    // Deferred-true while the target pass is torn down (parity); real failures
    // abort in the backend like the pre-migration VK_CHECK path.
    CreatePipeline(rhi);
    if (m_treeImpostor)
    {
        m_treeImpostor->renderer->SetTargetPass(m_targetPass);
        m_treeImpostor->renderer->RecreatePipeline(rhi);
    }
    return true;
}

std::size_t StaticMeshRenderer::TriangleCountForLod(std::uint64_t configHash, std::uint32_t lodLevel) const
{
    if (configHash == 0 || lodLevel == 0)
        return TriangleCount();
    const auto it = m_lodBuffers.find(configHash);
    if (it == m_lodBuffers.end() || it->second.levels == 0)
        return TriangleCount();
    const std::uint32_t level = std::min<std::uint32_t>(lodLevel, it->second.levels - 1u);
    return it->second.triangles[level] > 0 ? it->second.triangles[level] : TriangleCount();
}

StaticMeshRenderer::LodDiagnostics StaticMeshRenderer::GetLodDiagnostics(std::uint64_t configHash) const
{
    LodDiagnostics diag{};
    diag.vertexCount = m_vertices.size();
    if (configHash == 0)
    {
        diag.bufferKnown = true;
        diag.bufferValid = m_indexBuffer != nullptr;
        diag.levelCount = 1;
        diag.levelTris[0] = TriangleCount();
        diag.levelIndices[0] = m_indices.size();
        diag.source = "fullres";
        return diag;
    }

    const auto existing = m_lodBuffers.find(configHash);
    if (existing != m_lodBuffers.end())
    {
        diag.bufferKnown = true;
        diag.bufferValid = existing->second.buffer != nullptr;
        diag.levelCount = existing->second.levels;
        diag.levelTris = existing->second.triangles;
        const std::size_t drawsPerLevel = m_draws.size();
        for (std::uint32_t level = 0; level < existing->second.levels && level < LodConfig::MaxLevels; ++level)
        {
            const std::size_t begin = static_cast<std::size_t>(level) * drawsPerLevel;
            const std::size_t end = std::min(begin + drawsPerLevel, existing->second.draws.size());
            for (std::size_t i = begin; i < end; ++i)
                diag.levelIndices[level] += existing->second.draws[i].indexCount;
        }
        diag.source = LodSourceName(existing->second.source);
        return diag;
    }

    const auto pending = std::find_if(m_pendingLodUploads.begin(), m_pendingLodUploads.end(),
        [&](const PendingLodUpload& upload) { return upload.configHash == configHash; });
    if (pending != m_pendingLodUploads.end())
    {
        diag.bufferKnown = true;
        diag.pendingUpload = true;
        diag.bufferValid = false;
        diag.levelCount = pending->lodSet.levels;
        diag.levelTris = pending->lodSet.triangles;
        const std::size_t drawsPerLevel = m_draws.size();
        for (std::uint32_t level = 0; level < pending->lodSet.levels && level < LodConfig::MaxLevels; ++level)
        {
            const std::size_t begin = static_cast<std::size_t>(level) * drawsPerLevel;
            const std::size_t end = std::min(begin + drawsPerLevel, pending->lodSet.draws.size());
            for (std::size_t i = begin; i < end; ++i)
                diag.levelIndices[level] += pending->lodSet.draws[i].indexCount;
        }
        diag.source = LodSourceName(pending->lodSet.source);
        return diag;
    }

    return diag;
}

bool StaticMeshRenderer::CopyPhysicsMesh(
    std::vector<std::array<float, 3>>& outVertices,
    std::vector<std::uint32_t>& outIndices) const
{
    if (!IsLoaded() || m_vertices.empty() || m_indices.size() < 3)
        return false;

    outVertices.clear();
    outVertices.reserve(m_vertices.size());
    for (const Vertex& vertex : m_vertices)
        outVertices.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});

    outIndices = m_indices;
    return !outVertices.empty() && outIndices.size() >= 3;
}

bool StaticMeshRenderer::BakeTreeImpostorAsset(client::asset::IAssetReader& assets,
    const std::filesystem::path& modelPath, const tree_tool::TreeImpostorSettings& settings, std::string& error)
{
    const auto materials = AssetDatabase::Instance().loadDefaultMaterials(modelPath);
    StaticMeshRenderer source;
    source.m_isTreeImpostor = true; // Load just the original geometry, regardless of an earlier bake.
    if (materials.size() != 2 || !source.LoadCpu(assets, modelPath.generic_string()) ||
        source.m_draws.empty() || std::any_of(source.m_draws.begin(), source.m_draws.end(), [](const auto& draw) { return draw.materialSlot >= 2; }))
    { error = "A static tree with two default materials (bark/leaves) is required"; return false; }
    ixtreemetree::TreeMesh mesh;
    mesh.bboxMin = {source.m_boundsMin[0], source.m_boundsMin[1], source.m_boundsMin[2]};
    mesh.bboxMax = {source.m_boundsMax[0], source.m_boundsMax[1], source.m_boundsMax[2]};
    for (const auto& vertex : source.m_vertices)
    {
        const ixtreemetree::Vertex treeVertex{{vertex.position[0], vertex.position[1], vertex.position[2]},
            {vertex.normal[0], vertex.normal[1], vertex.normal[2]}, {vertex.uv[0], vertex.uv[1]}};
        mesh.bark.vertices.push_back(treeVertex); mesh.leaves.vertices.push_back(treeVertex);
    }
    for (const auto& draw : source.m_draws)
    {
        auto& indices = draw.materialSlot == 0 ? mesh.bark.indices : mesh.leaves.indices;
        indices.insert(indices.end(), source.m_indices.begin() + draw.firstIndex,
            source.m_indices.begin() + draw.firstIndex + draw.indexCount);
    }
    auto dependencies = AssetDatabase::Instance().loadDependencies(modelPath);
    if (!tree_tool::BakeTreeImpostor(mesh, modelPath, materials, settings, dependencies, error)) return false;
    return AssetDatabase::Instance().writeDependencies(modelPath, dependencies);
}

void StaticMeshRenderer::DumpMaterialState(const char* entityName, const Instance& instance) const
{
    LogFormat("[MATBIND-DIAG] === entity name=%s entityId=%u submeshCount=%zu ===",
        entityName && entityName[0] ? entityName : "<unnamed>",
        instance.entityId,
        m_draws.size());

    auto guidText = [](const std::optional<Guid>& guid) {
        return guid ? guid->toString() : std::string("EMPTY");
    };
    auto handleText = [](const std::string& name) {
        return name.empty() ? std::string("<none>") : name;
    };
    auto lastBindingFor = [&](std::uint32_t sourceSubmesh, std::uint32_t materialSlot) -> const LastMaterialBinding* {
        for (const LastMaterialBinding& binding : m_lastMaterialBindings)
        {
            if (binding.sourceSubmesh == sourceSubmesh && binding.materialSlot == materialSlot)
                return &binding;
        }
        return nullptr;
    };

    for (std::size_t i = 0; i < m_draws.size(); ++i)
    {
        const MeshDraw& draw = m_draws[i];
        const std::uint32_t materialSlot = draw.materialSlot;
        const std::string slotGuid =
            materialSlot < instance.materialSlots.size() ? instance.materialSlots[materialSlot] : std::string{};
        const MaterialDefaults bakedFallback{};
        const MaterialDefaults& baked = m_materialDefaults.empty()
            ? bakedFallback
            : (materialSlot < m_materialDefaults.size() ? m_materialDefaults[materialSlot] : m_materialDefaults.front());

        const char* source = "gltf_baked";
        std::string resolvedName = "NULL";
        const char* alphaMode = AlphaModeForLog(baked.alphaMode);
        float alphaCutoff = baked.alphaCutoff;
        float baseColor[4] = {baked.baseColor[0], baked.baseColor[1], baked.baseColor[2], baked.baseColor[3]};
        float metallic = baked.metallic;
        float roughness = baked.roughness;
        std::string baseColorTextureGuid = "EMPTY";
        std::string resolvedBaseColorView = handleText(m_texture.name);
        std::string descriptorSet = "NULL";
        std::string boundBeforeDraw = "no";

        if (slotGuid.empty())
        {
            source = "pink_fallback";
            if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
            {
                resolvedName = pink->name;
                baseColor[0] = pink->baseColor[0];
                baseColor[1] = pink->baseColor[1];
                baseColor[2] = pink->baseColor[2];
                baseColor[3] = pink->baseColor[3];
                metallic = pink->metallic;
                roughness = pink->roughness;
                alphaMode = MaterialAlphaModeName(pink->alphaMode);
                alphaCutoff = pink->alphaCutoff;
            }
        }
        else if (const std::optional<Guid> guid = Guid::fromString(slotGuid))
        {
            if (MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(*guid))
            {
                source = "slot";
                resolvedName = material->name.empty() ? material->path.filename().generic_string() : material->name;
                baseColor[0] = material->baseColor[0];
                baseColor[1] = material->baseColor[1];
                baseColor[2] = material->baseColor[2];
                baseColor[3] = material->baseColor[3];
                metallic = material->metallic;
                roughness = material->roughness;
                alphaMode = MaterialAlphaModeName(material->alphaMode);
                alphaCutoff = material->alphaCutoff;
                baseColorTextureGuid = guidText(material->baseColorTexture);
            }
            else
            {
                source = "pink_fallback";
                if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
                {
                    resolvedName = pink->name;
                    baseColor[0] = pink->baseColor[0];
                    baseColor[1] = pink->baseColor[1];
                    baseColor[2] = pink->baseColor[2];
                    baseColor[3] = pink->baseColor[3];
                    metallic = pink->metallic;
                    roughness = pink->roughness;
                    alphaMode = MaterialAlphaModeName(pink->alphaMode);
                    alphaCutoff = pink->alphaCutoff;
                }
                baseColorTextureGuid = slotGuid;
            }
        }
        else
        {
            source = "pink_fallback";
            if (MaterialAsset* pink = MaterialAssetManager::Instance().getOrLoad(MaterialAssetManager::PinkMissingMaterialGuid()))
            {
                resolvedName = pink->name;
                baseColor[0] = pink->baseColor[0];
                baseColor[1] = pink->baseColor[1];
                baseColor[2] = pink->baseColor[2];
                baseColor[3] = pink->baseColor[3];
                metallic = pink->metallic;
                roughness = pink->roughness;
                alphaMode = MaterialAlphaModeName(pink->alphaMode);
                alphaCutoff = pink->alphaCutoff;
            }
            baseColorTextureGuid = slotGuid;
        }

        if (const LastMaterialBinding* binding = lastBindingFor(static_cast<std::uint32_t>(i), materialSlot))
        {
            resolvedBaseColorView = handleText(binding->baseColorTexture);
            descriptorSet = "slot" + std::to_string(binding->bindSlot);
            boundBeforeDraw = binding->boundBeforeDraw ? "yes" : "no";
            alphaMode = binding->alphaMode.c_str();
            alphaCutoff = binding->alphaCutoff;
            if (binding->resolvedMaterial != "gltf_baked")
                resolvedName = binding->resolvedMaterial;
            if (binding->baseColorTextureGuid)
                baseColorTextureGuid = binding->baseColorTextureGuid->toString();
        }

        LogFormat("[MATBIND-DIAG]   submesh=%zu materialSlot=%u", i, materialSlot);
        LogFormat("[MATBIND-DIAG]     slotGuid=%s", slotGuid.empty() ? "EMPTY" : slotGuid.c_str());
        LogFormat("[MATBIND-DIAG]     resolvedMaterial=%s source=%s", resolvedName.c_str(), source);
        LogFormat("[MATBIND-DIAG]     baseColorTextureGuid=%s", baseColorTextureGuid.c_str());
        LogFormat("[MATBIND-DIAG]     resolvedBaseColorView=%s", resolvedBaseColorView.c_str());
        LogFormat("[MATBIND-DIAG]     alphaMode=%s", alphaMode);
        LogFormat("[MATBIND-DIAG]     alphaCutoff=%.3f", alphaCutoff);
        LogFormat("[MATBIND-DIAG]     rhiPipeline=%s",
            lastBindingFor(static_cast<std::uint32_t>(i), materialSlot)
                ? lastBindingFor(static_cast<std::uint32_t>(i), materialSlot)->pipelineName.c_str()
                : "NULL");
        LogFormat("[MATBIND-DIAG]     fragmentShaderAlphaPath=%s",
            lastBindingFor(static_cast<std::uint32_t>(i), materialSlot)
                ? lastBindingFor(static_cast<std::uint32_t>(i), materialSlot)->fragmentShaderAlphaPath
                : AlphaFragmentPath(alphaMode));
        LogFormat("[MATBIND-DIAG]     descriptorSet=%s boundBeforeDraw=%s",
            descriptorSet.c_str(),
            boundBeforeDraw.c_str());
        LogFormat("[MATBIND-DIAG]     baseColor=(%.3f,%.3f,%.3f,%.3f) alphaMode=%s alphaCutoff=%.3f metallic=%.3f roughness=%.3f",
            baseColor[0], baseColor[1], baseColor[2], baseColor[3],
            alphaMode,
            alphaCutoff,
            metallic,
            roughness);
        LogFormat("[MATBIND-DIAG]     vertexCount=%u indexCount=%u drawCallIssued=%s",
            draw.vertexCount,
            draw.indexCount,
            draw.indexCount > 0 ? "yes" : "no");
    }
}

bool StaticMeshRenderer::LoadStaticGltfMesh(const std::string& modelPath)
{
    if (!m_assets)
        return false;
    std::string error;
    auto parsed = ParseGltf(*m_assets, modelPath, &error, false);
    if (!parsed)
    {
        LogFormat("[STATIC-MESH] parse failed: %s reason=%s", modelPath.c_str(), error.c_str());
        return false;
    }
    const fastgltf::Asset& asset = *parsed;
    if (AssetLooksSkinned(asset))
    {
        m_status = LoadStatus::UnsupportedSkinned;
        return false;
    }

    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_lodProxyBuilt = false;
    m_lodProxyIndices.clear();
    m_lodProxyDraws.clear();
    m_materialDefaults.clear();
    m_alphaModeName = "opaque";
    m_materialDefaults.reserve(std::max<std::size_t>(asset.materials.size(), 1u));
    if (asset.materials.empty())
    {
        m_materialDefaults.push_back(MaterialDefaults{});
    }
    else
    {
        for (const auto& material : asset.materials)
        {
            m_materialDefaults.push_back(ReadMaterialDefaults(material));
            if (material.alphaMode == fastgltf::AlphaMode::Blend)
                m_alphaModeName = "blend";
            else if (material.alphaMode == fastgltf::AlphaMode::Mask && m_alphaModeName != "blend")
                m_alphaModeName = "mask";
        }
    }
    m_materialSlotCount = static_cast<std::uint32_t>(std::max<std::size_t>(m_materialDefaults.size(), 1u));
    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    uint32_t primitiveIndex = 0;
    for (const auto& mesh : asset.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
                continue;
            auto posIt = primitive.findAttribute("POSITION");
            if (posIt == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
                continue;

            auto normalIt = primitive.findAttribute("NORMAL");
            auto uvIt = primitive.findAttribute("TEXCOORD_0");
            const auto& positionAccessor = asset.accessors[posIt->accessorIndex];
            const size_t vertexCount = positionAccessor.count;
            const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
            std::vector<Vertex> vertices(vertexCount);

            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, positionAccessor, [&](fastgltf::math::fvec3 value, size_t index)
                {
                    vertices[index].position[0] = value.x();
                    vertices[index].position[1] = value.y();
                    vertices[index].position[2] = value.z();
                    vertices[index].normal[1] = 1.0f;
                    m_boundsMin[0] = std::min(m_boundsMin[0], vertices[index].position[0]);
                    m_boundsMin[1] = std::min(m_boundsMin[1], vertices[index].position[1]);
                    m_boundsMin[2] = std::min(m_boundsMin[2], vertices[index].position[2]);
                    m_boundsMax[0] = std::max(m_boundsMax[0], vertices[index].position[0]);
                    m_boundsMax[1] = std::max(m_boundsMax[1], vertices[index].position[1]);
                    m_boundsMax[2] = std::max(m_boundsMax[2], vertices[index].position[2]);
                });
            if (normalIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[normalIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, accessor, [&](fastgltf::math::fvec3 value, size_t index)
                    {
                        vertices[index].normal[0] = value.x();
                        vertices[index].normal[1] = value.y();
                        vertices[index].normal[2] = value.z();
                    });
            }
            if (uvIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[uvIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                    asset, accessor, [&](fastgltf::math::fvec2 value, size_t index)
                    {
                        vertices[index].uv[0] = value.x();
                        vertices[index].uv[1] = value.y();
                    });
            }

            const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
            const auto& indexAccessor = asset.accessors[primitive.indicesAccessor.value()];
            fastgltf::iterateAccessor<std::uint32_t>(
                asset, indexAccessor, [&](std::uint32_t index)
                {
                    m_indices.push_back(baseVertex + index);
                });

            m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
            MeshDraw draw{};
            draw.firstIndex = firstIndex;
            draw.indexCount = static_cast<uint32_t>(m_indices.size() - firstIndex);
            draw.materialSlot = static_cast<uint32_t>(
                primitive.materialIndex.has_value() ? primitive.materialIndex.value() : 0u);
            if (draw.materialSlot >= m_materialSlotCount)
                draw.materialSlot = 0;
            draw.vertexCount = static_cast<uint32_t>(vertices.size());
            m_draws.push_back(draw);
            LogFormat("[STATIC-MESH] extracted primitive[%u] mesh='%s': verts=%zu indices=%u materialSlot=%u",
                primitiveIndex++, mesh.name.c_str(), vertices.size(), draw.indexCount, draw.materialSlot);
        }
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        LogFormat("[STATIC-MESH] no renderable static mesh data extracted: %s", modelPath.c_str());
        m_boundsMin = {0.0f, 0.0f, 0.0f};
        m_boundsMax = {0.0f, 0.0f, 0.0f};
        return false;
    }
    // Its materials are registered in FinishGpu (this may run on a loading thread); asset is not
    // read past here.
    m_pendingMaterialImport = std::make_unique<PendingMaterialImport>(PendingMaterialImport{std::move(*parsed)});
    LogFormat("[MPERF] mesh=%s verts=%zu submeshes=%zu materials=%u alpha=%s",
        modelPath.c_str(),
        m_vertices.size(),
        m_draws.size(),
        m_materialSlotCount,
        m_alphaModeName.c_str());
    return true;
}

bool StaticMeshRenderer::LoadBuiltinPrimitiveMesh(const std::string& modelPath)
{
    const std::string prefix = "builtin://primitive/";
    if (modelPath.rfind(prefix, 0) != 0)
        return false;
    const std::string type = modelPath.substr(prefix.size());

    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_lodProxyBuilt = false;
    m_lodProxyIndices.clear();
    m_lodProxyDraws.clear();
    m_materialDefaults.clear();
    m_materialDefaults.push_back(MaterialDefaults{});
    m_materialSlotCount = 1;
    m_alphaModeName = "opaque";
    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    auto addVertex = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = z;
        vertex.normal[0] = nx;
        vertex.normal[1] = ny;
        vertex.normal[2] = nz;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        m_boundsMin[0] = std::min(m_boundsMin[0], x);
        m_boundsMin[1] = std::min(m_boundsMin[1], y);
        m_boundsMin[2] = std::min(m_boundsMin[2], z);
        m_boundsMax[0] = std::max(m_boundsMax[0], x);
        m_boundsMax[1] = std::max(m_boundsMax[1], y);
        m_boundsMax[2] = std::max(m_boundsMax[2], z);
        m_vertices.push_back(vertex);
        return static_cast<std::uint32_t>(m_vertices.size() - 1u);
    };
    auto addTri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        m_indices.push_back(a);
        m_indices.push_back(b);
        m_indices.push_back(c);
    };
    auto addQuad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        addTri(a, b, c);
        addTri(a, c, d);
    };

    if (type == "cube")
    {
        struct Face
        {
            float normal[3];
            float corners[4][3];
        };
        constexpr float s = 0.5f;
        const std::array<Face, 6> faces{{
            {{0.0f, 0.0f, 1.0f}, {{-s, -s, s}, {s, -s, s}, {s, s, s}, {-s, s, s}}},
            {{0.0f, 0.0f, -1.0f}, {{s, -s, -s}, {-s, -s, -s}, {-s, s, -s}, {s, s, -s}}},
            {{1.0f, 0.0f, 0.0f}, {{s, -s, s}, {s, -s, -s}, {s, s, -s}, {s, s, s}}},
            {{-1.0f, 0.0f, 0.0f}, {{-s, -s, -s}, {-s, -s, s}, {-s, s, s}, {-s, s, -s}}},
            {{0.0f, 1.0f, 0.0f}, {{-s, s, s}, {s, s, s}, {s, s, -s}, {-s, s, -s}}},
            {{0.0f, -1.0f, 0.0f}, {{-s, -s, -s}, {s, -s, -s}, {s, -s, s}, {-s, -s, s}}},
        }};
        for (const Face& face : faces)
        {
            const std::uint32_t base = static_cast<std::uint32_t>(m_vertices.size());
            addVertex(face.corners[0][0], face.corners[0][1], face.corners[0][2], face.normal[0], face.normal[1], face.normal[2], 0.0f, 1.0f);
            addVertex(face.corners[1][0], face.corners[1][1], face.corners[1][2], face.normal[0], face.normal[1], face.normal[2], 1.0f, 1.0f);
            addVertex(face.corners[2][0], face.corners[2][1], face.corners[2][2], face.normal[0], face.normal[1], face.normal[2], 1.0f, 0.0f);
            addVertex(face.corners[3][0], face.corners[3][1], face.corners[3][2], face.normal[0], face.normal[1], face.normal[2], 0.0f, 0.0f);
            addQuad(base + 0u, base + 1u, base + 2u, base + 3u);
        }
    }
    else if (type == "sphere")
    {
        constexpr std::uint32_t rings = 24;
        constexpr std::uint32_t segments = 32;
        constexpr float radius = 0.5f;
        for (std::uint32_t y = 0; y <= rings; ++y)
        {
            const float v = static_cast<float>(y) / static_cast<float>(rings);
            const float theta = v * xm::Pi;
            const float sinTheta = xm::Sin(theta);
            const float cosTheta = xm::Cos(theta);
            for (std::uint32_t x = 0; x <= segments; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(segments);
                const float phi = u * xm::TwoPi;
                const float nx = xm::Cos(phi) * sinTheta;
                const float ny = cosTheta;
                const float nz = xm::Sin(phi) * sinTheta;
                addVertex(nx * radius, ny * radius, nz * radius, nx, ny, nz, u, v);
            }
        }
        const std::uint32_t stride = segments + 1u;
        for (std::uint32_t y = 0; y < rings; ++y)
        {
            for (std::uint32_t x = 0; x < segments; ++x)
            {
                const std::uint32_t a = y * stride + x;
                const std::uint32_t b = a + 1u;
                const std::uint32_t c = (y + 1u) * stride + x + 1u;
                const std::uint32_t d = (y + 1u) * stride + x;
                addQuad(a, b, c, d);
            }
        }
    }
    else if (type == "capsule")
    {
        constexpr std::uint32_t hemiRings = 8;
        constexpr std::uint32_t segments = 32;
        constexpr float radius = 0.5f;
        constexpr float halfCylinder = 0.5f;
        std::vector<std::uint32_t> ringStarts;
        auto addRing = [&](float y, float ringRadius, float normalCenterY, float v) {
            ringStarts.push_back(static_cast<std::uint32_t>(m_vertices.size()));
            for (std::uint32_t x = 0; x <= segments; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(segments);
                const float phi = u * xm::TwoPi;
                const float px = xm::Cos(phi) * ringRadius;
                const float pz = xm::Sin(phi) * ringRadius;
                float nx = px;
                float ny = y - normalCenterY;
                float nz = pz;
                const float len = std::max(0.0001f, xm::Sqrt(nx * nx + ny * ny + nz * nz));
                nx /= len;
                ny /= len;
                nz /= len;
                addVertex(px, y, pz, nx, ny, nz, u, v);
            }
        };
        for (std::uint32_t ring = 0; ring <= hemiRings; ++ring)
        {
            const float t = static_cast<float>(ring) / static_cast<float>(hemiRings);
            const float angle = t * xm::HalfPi;
            addRing(halfCylinder + xm::Cos(angle) * radius, xm::Sin(angle) * radius, halfCylinder, t * 0.25f);
        }
        addRing(-halfCylinder, radius, -halfCylinder, 0.75f);
        for (std::uint32_t ring = 1; ring <= hemiRings; ++ring)
        {
            const float t = static_cast<float>(ring) / static_cast<float>(hemiRings);
            const float angle = xm::HalfPi + t * xm::HalfPi;
            addRing(-halfCylinder + xm::Cos(angle) * radius, xm::Sin(angle) * radius, -halfCylinder, 0.75f + t * 0.25f);
        }
        const std::uint32_t stride = segments + 1u;
        for (std::size_t ring = 0; ring + 1u < ringStarts.size(); ++ring)
        {
            const std::uint32_t rowA = ringStarts[ring];
            const std::uint32_t rowB = ringStarts[ring + 1u];
            for (std::uint32_t x = 0; x < segments; ++x)
                addQuad(rowA + x, rowA + x + 1u, rowB + x + 1u, rowB + x);
        }
        (void)stride;
    }
    else
    {
        LogFormat("[PRIMITIVE] unknown builtin primitive: %s", modelPath.c_str());
        return false;
    }

    if (m_vertices.empty() || m_indices.empty())
        return false;

    MeshDraw draw{};
    draw.firstIndex = 0;
    draw.indexCount = static_cast<std::uint32_t>(m_indices.size());
    draw.materialSlot = 0;
    draw.vertexCount = static_cast<std::uint32_t>(m_vertices.size());
    m_draws.push_back(draw);
    LogFormat("[PRIMITIVE] generated type=%s verts=%zu indices=%zu tris=%zu",
        type.c_str(),
        m_vertices.size(),
        m_indices.size(),
        m_indices.size() / 3u);
    return true;
}

bool StaticMeshRenderer::LoadStaticFbxMesh(const std::string& modelPath)
{
    std::filesystem::path fbxPath(modelPath);
    if (fbxPath.is_relative())
    {
        if (m_assets)
        {
            if (auto root = m_assets->RootPath())
                fbxPath = *root / fbxPath;
            else
                fbxPath = std::filesystem::absolute(fbxPath);
        }
        else
        {
            fbxPath = std::filesystem::absolute(fbxPath);
        }
    }
    std::error_code ec;
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(fbxPath, ec);
    if (!ec)
        fbxPath = canonicalPath;

    AssimpImporter importer;
    AssimpImporter::ImportResult result = importer.importFile(fbxPath);
    if (!result.success)
    {
        LogFormat("[STATIC-MESH] FBX parse failed: %s reason=%s",
            modelPath.c_str(),
            result.errorMessage.empty() ? "unknown" : result.errorMessage.c_str());
        return false;
    }

    if (result.skeletalIgnored)
    {
        LogFormat("[FBX-IMPORT] skeletal data ignored for static renderer path=%s",
            fbxPath.generic_string().c_str());
    }

    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_lodProxyBuilt = false;
    m_lodProxyIndices.clear();
    m_lodProxyDraws.clear();
    m_materialDefaults.clear();
    m_alphaModeName = "opaque";
    m_materialDefaults.reserve(std::max<std::size_t>(result.materials.size(), 1u));
    if (result.materials.empty())
    {
        m_materialDefaults.push_back(MaterialDefaults{});
    }
    else
    {
        for (const GltfMaterialSource& source : result.materials)
        {
            MaterialDefaults defaults = MaterialDefaultsFromSource(source);
            m_materialDefaults.push_back(defaults);
            if (defaults.alphaMode == "blend" || defaults.alphaMode == "BLEND")
                m_alphaModeName = "blend";
            else if ((defaults.alphaMode == "mask" || defaults.alphaMode == "MASK") && m_alphaModeName != "blend")
                m_alphaModeName = "mask";
        }
    }
    m_materialSlotCount = static_cast<std::uint32_t>(std::max<std::size_t>(m_materialDefaults.size(), 1u));
    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    std::uint32_t meshIndex = 0;
    for (const AssimpImporter::StaticMeshData& mesh : result.meshes)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
            continue;

        const std::uint32_t baseVertex = static_cast<std::uint32_t>(m_vertices.size());
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(m_indices.size());
        for (const AssimpImporter::Vertex& src : mesh.vertices)
        {
            Vertex vertex{};
            vertex.position[0] = src.position[0];
            vertex.position[1] = src.position[1];
            vertex.position[2] = src.position[2];
            vertex.normal[0] = src.normal[0];
            vertex.normal[1] = src.normal[1];
            vertex.normal[2] = src.normal[2];
            vertex.uv[0] = src.uv[0];
            vertex.uv[1] = src.uv[1];
            m_boundsMin[0] = std::min(m_boundsMin[0], vertex.position[0]);
            m_boundsMin[1] = std::min(m_boundsMin[1], vertex.position[1]);
            m_boundsMin[2] = std::min(m_boundsMin[2], vertex.position[2]);
            m_boundsMax[0] = std::max(m_boundsMax[0], vertex.position[0]);
            m_boundsMax[1] = std::max(m_boundsMax[1], vertex.position[1]);
            m_boundsMax[2] = std::max(m_boundsMax[2], vertex.position[2]);
            m_vertices.push_back(vertex);
        }
        for (std::uint32_t index : mesh.indices)
            m_indices.push_back(baseVertex + index);

        MeshDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = static_cast<std::uint32_t>(m_indices.size() - firstIndex);
        draw.materialSlot = mesh.materialIndex < m_materialSlotCount ? mesh.materialIndex : 0u;
        draw.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        m_draws.push_back(draw);
        LogFormat("[STATIC-MESH] extracted FBX mesh[%u] mesh='%s': verts=%zu indices=%u materialSlot=%u",
            meshIndex++,
            mesh.name.c_str(),
            mesh.vertices.size(),
            draw.indexCount,
            draw.materialSlot);
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        LogFormat("[STATIC-MESH] no renderable static FBX mesh data extracted: %s", modelPath.c_str());
        m_boundsMin = {0.0f, 0.0f, 0.0f};
        m_boundsMax = {0.0f, 0.0f, 0.0f};
        return false;
    }

    LogFormat("[FBX-IMPORT] runtime loaded path=%s meshes=%zu materials=%zu",
        fbxPath.generic_string().c_str(),
        result.meshes.size(),
        result.materials.size());
    LogFormat("[MPERF] mesh=%s verts=%zu submeshes=%zu materials=%u alpha=%s",
        modelPath.c_str(),
        m_vertices.size(),
        m_draws.size(),
        m_materialSlotCount,
        m_alphaModeName.c_str());
    return true;
}

bool StaticMeshRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    const std::uint64_t vertexBytes = sizeof(Vertex) * m_vertices.size();
    const std::uint64_t indexBytes = sizeof(uint32_t) * m_indices.size();
    if (m_deferUploads && m_pendingUploads)
    {
        // Made empty, the data staged (RecordPendingUploads copies it in).
        m_vertexBuffer = CreateRhiBuffer(rhi,
            vertexBytes,
            ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferDst,
            ixrhi::IXRHICpuAccess::None,
            nullptr,
            ("StaticMesh:" + m_modelPath + ":VB").c_str());
        m_indexBuffer = CreateRhiBuffer(rhi,
            indexBytes,
            ixrhi::IXRHIBufferUsage::Index | ixrhi::IXRHIBufferUsage::TransferDst,
            ixrhi::IXRHICpuAccess::None,
            nullptr,
            ("StaticMesh:" + m_modelPath + ":IB").c_str());
        m_pendingUploads->vertexStaging = CreateRhiBuffer(rhi,
            vertexBytes,
            ixrhi::IXRHIBufferUsage::TransferSrc,
            ixrhi::IXRHICpuAccess::Write,
            m_vertices.data(),
            ("StaticMesh:" + m_modelPath + ":VBStaging").c_str());
        m_pendingUploads->indexStaging = CreateRhiBuffer(rhi,
            indexBytes,
            ixrhi::IXRHIBufferUsage::TransferSrc,
            ixrhi::IXRHICpuAccess::Write,
            m_indices.data(),
            ("StaticMesh:" + m_modelPath + ":IBStaging").c_str());
        if (!m_pendingUploads->vertexStaging || !m_pendingUploads->indexStaging)
            return false;
    }
    else
    {
        m_vertexBuffer = CreateRhiBuffer(rhi,
            vertexBytes,
            ixrhi::IXRHIBufferUsage::Vertex,
            ixrhi::IXRHICpuAccess::None,
            m_vertices.data(),
            ("StaticMesh:" + m_modelPath + ":VB").c_str());
        m_indexBuffer = CreateRhiBuffer(rhi,
            indexBytes,
            ixrhi::IXRHIBufferUsage::Index,
            ixrhi::IXRHICpuAccess::None,
            m_indices.data(),
            ("StaticMesh:" + m_modelPath + ":IB").c_str());
    }
    if (!m_vertexBuffer || !m_indexBuffer)
        return false;
    // The shadow detail levels (none made: the model's own triangles in every cascade).
    m_lodIndexBuffer.reset();
    m_shadowCardVertexBuffer.reset();
    const bool deferred = m_deferUploads && m_pendingUploads;
    const auto makeShadowLodBuffer = [&](const void* data, std::uint64_t bytes, ixrhi::IXRHIBufferUsage usage,
                                         std::shared_ptr<ixrhi::IXRHIBuffer>* staging, const char* name) {
        std::shared_ptr<ixrhi::IXRHIBuffer> buffer = CreateRhiBuffer(rhi,
            bytes,
            deferred ? usage | ixrhi::IXRHIBufferUsage::TransferDst : usage,
            ixrhi::IXRHICpuAccess::None,
            deferred ? nullptr : data,
            ("StaticMesh:" + m_modelPath + ":" + name).c_str());
        if (buffer && deferred)
        {
            *staging = CreateRhiBuffer(rhi, bytes, ixrhi::IXRHIBufferUsage::TransferSrc, ixrhi::IXRHICpuAccess::Write,
                data, ("StaticMesh:" + m_modelPath + ":" + name + "Staging").c_str());
            if (!*staging)
                buffer.reset();
        }
        return buffer;
    };
    if (!m_lodIndices.empty())
        m_lodIndexBuffer = makeShadowLodBuffer(m_lodIndices.data(),
            sizeof(std::uint32_t) * m_lodIndices.size(), ixrhi::IXRHIBufferUsage::Index,
            deferred ? &m_pendingUploads->lodIndexStaging : nullptr, "ShadowLodIB");
    if (m_lodIndexBuffer && !m_shadowCardVertices.empty())
        m_shadowCardVertexBuffer = makeShadowLodBuffer(m_shadowCardVertices.data(),
            sizeof(Vertex) * m_shadowCardVertices.size(), ixrhi::IXRHIBufferUsage::Vertex,
            deferred ? &m_pendingUploads->shadowCardVertexStaging : nullptr, "ShadowCardVB");
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        m_instanceBufferCapacity[frame] = kInitialInstanceCapacity;
        m_instanceBuffers[frame] = CreateRhiBuffer(rhi,
            sizeof(InstanceBlock) * m_instanceBufferCapacity[frame],
            ixrhi::IXRHIBufferUsage::Storage,
            ixrhi::IXRHICpuAccess::Write,
            nullptr,
            ("StaticMesh:" + m_modelPath + ":Instances").c_str());
        if (!m_instanceBuffers[frame])
            return false;
    }
    return true;
}

std::filesystem::path StaticMeshRenderer::LodCachePath(const std::string& modelPath, std::uint64_t configHash) const
{
    std::filesystem::path assetPath(modelPath);
    if (assetPath.is_relative() && m_assets)
    {
        if (auto root = m_assets->RootPath())
            assetPath = *root / assetPath;
    }
    if (assetPath.empty())
        return {};
    const std::filesystem::path cacheDir = assetPath.parent_path() / ".lod_cache";
    const std::string filename = assetPath.stem().string() + "_" + std::to_string(configHash) + ".lodbin";
    return cacheDir / filename;
}

bool StaticMeshRenderer::EnsureLodBuffers(const LodConfig& config, std::uint64_t configHash, std::uint32_t entityId)
{
    if (configHash == 0 || m_vertices.empty() || m_indices.empty() || m_draws.empty())
        return false;
    ApplyPendingLodResult(configHash);
    auto cached = m_lodBuffers.find(configHash);
    if (cached != m_lodBuffers.end())
        return cached->second.buffer != nullptr;
    if (HasPendingLodUpload(configHash))
        return false;

    const std::filesystem::path cachePath = LodCachePath(m_modelPath, configHash);
    if (!cachePath.empty())
    {
        LodCpuSet cachedCpu;
        if (LoadLodCpuCache(configHash, cachedCpu))
        {
            cachedCpu.diagnosticEntityId = entityId;
            std::lock_guard<std::mutex> lock(m_lodMutex);
            m_lodPendingResults[configHash] = std::move(cachedCpu);
        }
        if (ApplyPendingLodResult(configHash))
            return true;
    }

    RequestLodBuild(config, configHash, false, entityId);
    return false;
}

bool StaticMeshRenderer::EnsureLodPreviewProxy()
{
    if (m_lodProxyBuilt)
        return !m_lodProxyIndices.empty() && !m_lodProxyDraws.empty();
    const auto begin = std::chrono::steady_clock::now();
    constexpr std::size_t kTargetPreviewTriangles = 150000;
    const std::size_t sourceTriangles = m_indices.size() / 3u;
    if (sourceTriangles == 0 || m_draws.empty())
        return false;

    const float proxyRatio = sourceTriangles <= kTargetPreviewTriangles
        ? 1.0f
        : static_cast<float>(kTargetPreviewTriangles) / static_cast<float>(sourceTriangles);
    m_lodProxyIndices.clear();
    m_lodProxyDraws.clear();
    m_lodProxyIndices.reserve(std::min<std::size_t>(m_indices.size(), kTargetPreviewTriangles * 3u));
    m_lodProxyDraws.reserve(m_draws.size());

    for (std::uint32_t drawIndex = 0; drawIndex < static_cast<std::uint32_t>(m_draws.size()); ++drawIndex)
    {
        const MeshDraw& draw = m_draws[drawIndex];
        std::vector<std::uint32_t> proxy;
        if (proxyRatio >= 0.999f)
        {
            proxy.assign(m_indices.begin() + draw.firstIndex, m_indices.begin() + draw.firstIndex + draw.indexCount);
        }
        else
        {
            std::size_t targetIndexCount = static_cast<std::size_t>(static_cast<float>(draw.indexCount) * proxyRatio);
            targetIndexCount = std::max<std::size_t>(3u, (targetIndexCount / 3u) * 3u);
            targetIndexCount = std::min<std::size_t>(draw.indexCount, targetIndexCount);
            proxy.resize(draw.indexCount);
            const std::size_t written = meshopt_simplifySloppy(proxy.data(),
                m_indices.data() + draw.firstIndex,
                draw.indexCount,
                &m_vertices.front().position[0],
                m_vertices.size(),
                sizeof(Vertex),
                targetIndexCount,
                1e-2f);
            proxy.resize(std::max<std::size_t>(3u, (written / 3u) * 3u));
        }
        if (proxy.empty())
            continue;

        MeshDraw proxyDraw{};
        proxyDraw.firstIndex = static_cast<std::uint32_t>(m_lodProxyIndices.size());
        proxyDraw.indexCount = static_cast<std::uint32_t>(proxy.size());
        proxyDraw.materialSlot = draw.materialSlot;
        m_lodProxyIndices.insert(m_lodProxyIndices.end(), proxy.begin(), proxy.end());
        m_lodProxyDraws.push_back(proxyDraw);
    }

    m_lodProxyBuilt = true;
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
    LogFormat("[LOD-ASYNC] proxy built tris=%zu ms=%.3f",
        m_lodProxyIndices.size() / 3u,
        ms);
    return !m_lodProxyIndices.empty() && !m_lodProxyDraws.empty();
}

StaticMeshRenderer::LodCpuSet StaticMeshRenderer::BuildLodCpuSet(const LodConfig& config,
    std::uint64_t configHash,
    bool quality,
    std::uint32_t entityId)
{
    static std::atomic_bool meshoptBuildLogged = false;
    if (!meshoptBuildLogged.exchange(true))
    {
#if defined(_DEBUG)
        constexpr const char* kBuildConfig = "Debug";
#else
        constexpr const char* kBuildConfig = "Release";
#endif
#if defined(IXTREEME_MESHOPT_FETCHCONTENT) && defined(IXTREEME_MESHOPT_OPTIMIZED_DEBUG)
        LogFormat("[LOD-BUILD] meshopt source=fetchcontent optimized=yes config=%s", kBuildConfig);
#else
        LogFormat("[LOD-BUILD] meshopt source=unknown optimized=no config=%s", kBuildConfig);
#endif
    }

    const auto begin = std::chrono::steady_clock::now();
    LodCpuSet lodSet{};
    lodSet.config = config;
    lodSet.configHash = configHash;
    lodSet.quality = quality;
    lodSet.writeCache = quality;
    lodSet.source = quality ? LodBufferSource::Commit : LodBufferSource::Preview;
    lodSet.diagnosticEntityId = entityId;
    lodSet.levels = std::clamp(config.levelCount, 1u, LodConfig::MaxLevels);
    const bool useProxy = !quality && EnsureLodPreviewProxy();
    const std::vector<std::uint32_t>& sourceIndices = useProxy ? m_lodProxyIndices : m_indices;
    const std::vector<MeshDraw>& sourceDraws = useProxy ? m_lodProxyDraws : m_draws;
    lodSet.indices.reserve(sourceIndices.size() * lodSet.levels);
    lodSet.draws.reserve(sourceDraws.size() * lodSet.levels);
    std::array<std::vector<std::uint32_t>, LodConfig::MaxLevels> levelIndices{};
    std::array<std::vector<LodMeshDraw>, LodConfig::MaxLevels> levelDraws{};

    for (std::uint32_t level = 0; level < lodSet.levels; ++level)
    {
        const std::size_t levelInTris = sourceIndices.size() / 3u;
        for (std::uint32_t drawIndex = 0; drawIndex < static_cast<std::uint32_t>(sourceDraws.size()); ++drawIndex)
        {
            const MeshDraw& draw = sourceDraws[drawIndex];
            const std::size_t submeshInTris = draw.indexCount / 3u;
            std::vector<std::uint32_t> simplified;
            if (level == 0)
            {
                simplified.assign(sourceIndices.begin() + draw.firstIndex,
                    sourceIndices.begin() + draw.firstIndex + draw.indexCount);
            }
            else
            {
                const float ratio = std::clamp(config.targetRatios[level], 0.001f, 1.0f);
                std::size_t targetIndexCount = static_cast<std::size_t>(static_cast<float>(draw.indexCount) * ratio);
                targetIndexCount = std::max<std::size_t>(3u, (targetIndexCount / 3u) * 3u);
                targetIndexCount = std::min<std::size_t>(draw.indexCount, targetIndexCount);
                simplified.resize(draw.indexCount);
                const std::size_t written = quality
                    ? meshopt_simplify(simplified.data(),
                        sourceIndices.data() + draw.firstIndex,
                        draw.indexCount,
                        &m_vertices.front().position[0],
                        m_vertices.size(),
                        sizeof(Vertex),
                        targetIndexCount,
                        1e-2f,
                        0)
                    : meshopt_simplifySloppy(simplified.data(),
                        sourceIndices.data() + draw.firstIndex,
                        draw.indexCount,
                        &m_vertices.front().position[0],
                        m_vertices.size(),
                        sizeof(Vertex),
                        targetIndexCount,
                        1e-2f);
                simplified.resize(std::max<std::size_t>(3u, (written / 3u) * 3u));
                if (simplified.empty())
                {
                    simplified.assign(sourceIndices.begin() + draw.firstIndex,
                        sourceIndices.begin() + draw.firstIndex + draw.indexCount);
                }
            }

            LodMeshDraw lodDraw{};
            lodDraw.firstIndex = static_cast<std::uint32_t>(levelIndices[level].size());
            lodDraw.indexCount = static_cast<std::uint32_t>(simplified.size());
            lodDraw.materialSlot = draw.materialSlot;
            lodDraw.sourceDraw = drawIndex;
            levelIndices[level].insert(levelIndices[level].end(), simplified.begin(), simplified.end());
            levelDraws[level].push_back(lodDraw);
            const std::size_t submeshOutTris = simplified.size() / 3u;
            lodSet.triangles[level] += submeshOutTris;
            LogFormat("[LOD-GEN] entity=%u path=%s level=%u submesh=%u targetRatio=%.5f inTris=%zu outTris=%zu material=%s%s",
                entityId,
                quality ? "quality" : "sloppy",
                level,
                drawIndex,
                level == 0 ? 1.0f : std::clamp(config.targetRatios[level], 0.001f, 1.0f),
                submeshInTris,
                submeshOutTris,
                m_alphaModeName.c_str(),
                submeshOutTris < 12u ? " DEGENERATE" : "");
        }
        LogFormat("[LOD-GEN] entity=%u path=%s level=%u targetRatio=%.5f inTris=%zu outTris=%zu%s",
            entityId,
            quality ? "quality" : "sloppy",
            level,
            level == 0 ? 1.0f : std::clamp(config.targetRatios[level], 0.001f, 1.0f),
            levelInTris,
            lodSet.triangles[level],
            lodSet.triangles[level] < 12u ? " DEGENERATE" : "");
        LogFormat("[LOD-GEN] entity=%u level=%u ratio=%.5f requestedTris=%zu resultVerts=%zu resultIndices=%zu result=%s",
            entityId,
            level,
            level == 0 ? 1.0f : std::clamp(config.targetRatios[level], 0.001f, 1.0f),
            level == 0 ? levelInTris : static_cast<std::size_t>(static_cast<float>(levelInTris) * std::clamp(config.targetRatios[level], 0.001f, 1.0f)),
            m_vertices.size(),
            levelIndices[level].size(),
            levelIndices[level].empty() ? "EMPTY" : "OK");
    }

    for (std::uint32_t level = 0; level < lodSet.levels; ++level)
    {
        const std::uint32_t baseIndex = static_cast<std::uint32_t>(lodSet.indices.size());
        for (LodMeshDraw draw : levelDraws[level])
        {
            draw.firstIndex += baseIndex;
            lodSet.draws.push_back(draw);
        }
        lodSet.indices.insert(lodSet.indices.end(), levelIndices[level].begin(), levelIndices[level].end());
    }

    const auto end = std::chrono::steady_clock::now();
    lodSet.decimateMs = std::chrono::duration<double, std::milli>(end - begin).count();
    return lodSet;
}

bool StaticMeshRenderer::ApplyPendingLodResult(std::uint64_t configHash)
{
    if (PollPendingLodUploads(configHash))
        return true;

    LodCpuSet cpuSet;
    {
        std::lock_guard<std::mutex> lock(m_lodMutex);
        auto pending = m_lodPendingResults.find(configHash);
        if (pending == m_lodPendingResults.end())
            return false;
        cpuSet = std::move(pending->second);
        m_lodPendingResults.erase(pending);
    }

    if (cpuSet.indices.empty())
        return false;

    return QueueLodUpload(configHash, std::move(cpuSet));
}

bool StaticMeshRenderer::QueueLodUpload(std::uint64_t configHash, LodCpuSet&& cpuSet)
{
    if (!m_rhi || cpuSet.indices.empty() || HasPendingLodUpload(configHash))
        return false;

    const auto begin = std::chrono::steady_clock::now();
    ixrhi::IXRHIBufferDesc desc;
    desc.sizeBytes = sizeof(std::uint32_t) * cpuSet.indices.size();
    desc.usage = ixrhi::IXRHIBufferUsage::Index;
    desc.cpuAccess = ixrhi::IXRHICpuAccess::None;
    desc.debugName = "StaticMesh:" + m_modelPath + ":LOD";

    PendingLodUpload upload{};
    upload.configHash = configHash;
    upload.diagnosticEntityId = cpuSet.diagnosticEntityId;
    upload.lodSet.indices = std::move(cpuSet.indices);
    upload.lodSet.draws = std::move(cpuSet.draws);
    upload.lodSet.triangles = cpuSet.triangles;
    upload.lodSet.levels = cpuSet.levels;
    upload.lodSet.quality = cpuSet.quality;
    upload.lodSet.source = cpuSet.source;
    upload.lodSet.generated = true;
    upload.upload = m_rhi->UploadBufferAsync(desc,
        upload.lodSet.indices.data(),
        desc.sizeBytes);
    if (!upload.upload)
        return false;

    const auto end = std::chrono::steady_clock::now();
    upload.uploadMs = std::chrono::duration<double, std::milli>(end - begin).count();
    LogFormat("[LOD-ASYNC] swap uploadMs=%.3f blocking=no", upload.uploadMs);
    m_pendingLodUploads.push_back(std::move(upload));
    return false;
}

bool StaticMeshRenderer::PollPendingLodUploads(std::uint64_t configHash)
{
    bool appliedRequested = false;
    for (auto it = m_pendingLodUploads.begin(); it != m_pendingLodUploads.end();)
    {
        if (!it->upload || !it->upload->IsReady())
        {
            ++it;
            continue;
        }
        it->lodSet.buffer = it->upload->Take();
        it->upload.reset();

        const std::uint32_t populatedEntityId = it->diagnosticEntityId;
        const std::uint32_t populatedLevelCount = it->lodSet.levels;
        auto existing = m_lodBuffers.find(it->configHash);
        if (existing != m_lodBuffers.end())
        {
            // Retain the replaced buffer (shared ownership) so in-flight frames
            // referencing it stay valid — same as the old retired-buffer list.
            m_retiredLodBuffers.push_back(existing->second.buffer);
            existing->second = std::move(it->lodSet);
        }
        else
        {
            m_lodBuffers.emplace(it->configHash, std::move(it->lodSet));
        }
        LogFormat("[LOD-ASYNC] swap applied frame=%u", m_worldRenderFrameIndex);
        LogFormat("[LOD-LEVELS] populated entity=%u levelCount=%u",
            populatedEntityId,
            populatedLevelCount);
        if (it->configHash == configHash)
            appliedRequested = true;
        it = m_pendingLodUploads.erase(it);
    }
    return appliedRequested || m_lodBuffers.find(configHash) != m_lodBuffers.end();
}

bool StaticMeshRenderer::HasPendingLodUpload(std::uint64_t configHash) const
{
    return std::any_of(m_pendingLodUploads.begin(), m_pendingLodUploads.end(),
        [&](const PendingLodUpload& upload) { return upload.configHash == configHash; });
}

bool StaticMeshRenderer::LoadLodCpuCache(std::uint64_t configHash, LodCpuSet& out) const
{
    const std::filesystem::path cachePath = LodCachePath(m_modelPath, configHash);
    if (cachePath.empty())
        return false;
    std::ifstream in(cachePath, std::ios::binary);
    if (!in)
        return false;

    char magic[8]{};
    in.read(magic, sizeof(magic));
    std::uint32_t drawCount = 0;
    std::uint32_t indexCount = 0;
    if (std::strncmp(magic, "IWLOD2", 6) != 0 ||
        !ReadBinary(in, out.levels) ||
        !ReadBinary(in, drawCount) ||
        !ReadBinary(in, indexCount))
    {
        return false;
    }
    out.configHash = configHash;
    out.quality = true;
    out.source = LodBufferSource::Cache;
    out.levels = std::clamp(out.levels, 1u, LodConfig::MaxLevels);
    for (std::size_t& tris : out.triangles)
        ReadBinary(in, tris);
    out.draws.resize(drawCount);
    out.indices.resize(indexCount);
    if (!out.draws.empty())
        in.read(reinterpret_cast<char*>(out.draws.data()), sizeof(LodMeshDraw) * out.draws.size());
    if (!out.indices.empty())
        in.read(reinterpret_cast<char*>(out.indices.data()), sizeof(std::uint32_t) * out.indices.size());
    if (!in || out.indices.empty())
        return false;
    LogFormat("[LOD] generated asset=%s configHash=%llu submesh=all levels=%u tris=[%zu,%zu,%zu,%zu] source=cache",
        m_modelPath.c_str(),
        static_cast<unsigned long long>(configHash),
        out.levels,
        out.triangles[0],
        out.triangles[1],
        out.triangles[2],
        out.triangles[3]);
    return true;
}

void StaticMeshRenderer::WriteLodCpuCache(const LodCpuSet& set) const
{
    const std::filesystem::path cachePath = LodCachePath(m_modelPath, set.configHash);
    if (cachePath.empty() || set.indices.empty())
        return;
    const auto begin = std::chrono::steady_clock::now();
    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    std::ofstream out(cachePath, std::ios::binary);
    if (out)
    {
        char magic[8] = {'I', 'W', 'L', 'O', 'D', '2', '\0', '\0'};
        out.write(magic, sizeof(magic));
        const std::uint32_t drawCount = static_cast<std::uint32_t>(set.draws.size());
        const std::uint32_t indexCount = static_cast<std::uint32_t>(set.indices.size());
        WriteBinary(out, set.levels);
        WriteBinary(out, drawCount);
        WriteBinary(out, indexCount);
        for (const std::size_t tris : set.triangles)
            WriteBinary(out, tris);
        if (!set.draws.empty())
            out.write(reinterpret_cast<const char*>(set.draws.data()), sizeof(LodMeshDraw) * set.draws.size());
        if (!set.indices.empty())
            out.write(reinterpret_cast<const char*>(set.indices.data()), sizeof(std::uint32_t) * set.indices.size());
    }
    const auto end = std::chrono::steady_clock::now();
    const double cacheWriteMs = std::chrono::duration<double, std::milli>(end - begin).count();
    LogFormat("[LOD-REGEN] trigger=commit path=quality decimateMs=%.3f cacheWriteMs=%.3f",
        set.decimateMs,
        cacheWriteMs);
}

void StaticMeshRenderer::StartLodWorker()
{
    if (m_lodWorker.joinable())
        return;
    m_lodWorkerStop = false;
    m_lodWorker = std::thread([this]() { LodWorkerMain(); });
}

void StaticMeshRenderer::StopLodWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_lodMutex);
        m_lodWorkerStop = true;
        m_lodRequestPending = false;
    }
    m_lodCv.notify_all();
    if (m_lodWorker.joinable())
        m_lodWorker.join();
    {
        std::lock_guard<std::mutex> lock(m_lodMutex);
        m_lodPendingResults.clear();
        m_lodRunningHash = 0;
        m_lodRunningQuality = false;
        m_lodCoalescedDropped = 0;
    }
}

void StaticMeshRenderer::RequestLodBuild(const LodConfig& config,
    std::uint64_t configHash,
    bool quality,
    std::uint32_t entityId)
{
    if (configHash == 0)
        return;
    auto existing = m_lodBuffers.find(configHash);
    if (existing != m_lodBuffers.end() && (!quality || existing->second.quality))
        return;
    {
        std::lock_guard<std::mutex> lock(m_lodMutex);
        if (!quality)
        {
            if (m_lodRunningHash == configHash && m_lodRunningQuality)
                return;
            if (m_lodRequestPending && m_lodRequest.configHash == configHash && m_lodRequest.quality)
                return;
            const auto pendingResult = m_lodPendingResults.find(configHash);
            if (pendingResult != m_lodPendingResults.end() && pendingResult->second.quality)
                return;
        }
        if (m_lodRunningHash == configHash && m_lodRunningQuality == quality)
            return;
        if (m_lodRequestPending &&
            (m_lodRequest.configHash != configHash || m_lodRequest.quality != quality))
        {
            ++m_lodCoalescedDropped;
        }
        if (quality)
        {
            LogFormat("[LOD-LEVELS] commit start entity=%u configHash=%llu",
                entityId,
                static_cast<unsigned long long>(configHash));
        }
        m_lodRequest = {config, configHash, quality, entityId};
        m_lodRequestPending = true;
        LogFormat("[LOD-ASYNC] request configHash=%llu coalescedDropped=%u",
            static_cast<unsigned long long>(configHash),
            m_lodCoalescedDropped);
    }
    StartLodWorker();
    m_lodCv.notify_one();
}

void StaticMeshRenderer::RequestLodQualityBuild(const LodConfig& lodConfig,
    std::uint64_t configHash,
    std::uint32_t entityId)
{
    RequestLodBuild(lodConfig, configHash, true, entityId);
}

void StaticMeshRenderer::LodWorkerMain()
{
    for (;;)
    {
        LodBuildRequest request;
        {
            std::unique_lock<std::mutex> lock(m_lodMutex);
            m_lodCv.wait(lock, [&]() { return m_lodWorkerStop || m_lodRequestPending; });
            if (m_lodWorkerStop)
                return;
            request = m_lodRequest;
            m_lodRequestPending = false;
            m_lodRunningHash = request.configHash;
            m_lodRunningQuality = request.quality;
        }

        LodCpuSet result;
        bool fromCache = false;
        if (request.quality)
            fromCache = LoadLodCpuCache(request.configHash, result);
        if (!fromCache)
            result = BuildLodCpuSet(request.config, request.configHash, request.quality, request.diagnosticEntityId);
        else
            result.diagnosticEntityId = request.diagnosticEntityId;
        if (request.quality && !fromCache)
            WriteLodCpuCache(result);

        bool publish = !result.indices.empty();
        const double decimateMs = fromCache ? 0.0 : result.decimateMs;
        {
            std::lock_guard<std::mutex> lock(m_lodMutex);
            if (!request.quality && m_lodRequestPending && m_lodRequest.configHash != request.configHash)
                publish = false;
            if (publish)
                m_lodPendingResults[request.configHash] = std::move(result);
            m_lodRunningHash = 0;
            m_lodRunningQuality = false;
        }
        LogFormat("[LOD-LEVELS] result entity=%u configHash=%llu applied=%s reason=%s",
            request.diagnosticEntityId,
            static_cast<unsigned long long>(request.configHash),
            publish ? "y" : "n",
            publish ? "applied" : "stale-config-moved-on");
        if (request.quality)
        {
            LogFormat("[LOD-ASYNC] worker path=quality decimateMs=%.3f published=%s",
                decimateMs,
                publish ? "yes" : "no");
        }
        else
        {
            LogFormat("[LOD-ASYNC] worker path=sloppy previewIterMs=%.3f decimateMs=%.3f published=%s",
                decimateMs,
                decimateMs,
                publish ? "yes" : "no");
        }
    }
}

void StaticMeshRenderer::RecordTextureUpload(ixrhi::IXRHICommandList& cmd, const Texture& texture,
                                             const ixrhi::IXRHIBuffer& staging)
{
    cmd.TransitionTexture(*texture.image, ixrhi::IXRHIImageLayout::Undefined, ixrhi::IXRHIImageLayout::TransferDst);
    cmd.CopyBufferToTexture(staging, 0, texture.width, *texture.image, 0, 0, 0, 0, texture.width, texture.height);
    cmd.TransitionTexture(*texture.image, ixrhi::IXRHIImageLayout::TransferDst, ixrhi::IXRHIImageLayout::ShaderReadOnly);
}

void StaticMeshRenderer::RecordPendingUploads(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
{
    if (m_treeImpostor) m_treeImpostor->renderer->RecordPendingUploads(cmd, frame);
    if (!m_rhi || !frame.frameActive)
        return;
    // The material textures decoded since: made, and copied in ahead of the frame's draws.
    for (auto it = m_materialTextureLoads.begin(); it != m_materialTextureLoads.end();)
    {
        MaterialTextureLoad& load = *it->second;
        if (!load.decoded.Done())
        {
            ++it;
            continue;
        }
        Texture texture{};
        std::shared_ptr<ixrhi::IXRHIBuffer> staging;
        if (!load.ok || !UploadTexture(*m_rhi, load.image, texture, &staging) || !staging)
        {
            m_failedMaterialTextureKeys.insert(it->first);
            LogFormat("[MATBIND-DIAG] texture %s failed role=%s path=%s", load.ok ? "upload" : "decode",
                load.role.c_str(), load.path.generic_string().c_str());
            it = m_materialTextureLoads.erase(it);
            continue;
        }
        RecordTextureUpload(cmd, texture, *staging);
        m_materialTextureStaging.push_back({std::move(staging), frame.frameNumber});
        LogFormat("[MATBIND-DIAG] texture loaded role=%s image=%s path=%s", load.role.c_str(), texture.name.c_str(),
            load.path.generic_string().c_str());
        m_materialTextureCache.emplace(it->first, std::move(texture));
        it = m_materialTextureLoads.erase(it);
    }
    // Staging whose copy's frame is done (its slot came round again).
    m_materialTextureStaging.erase(std::remove_if(m_materialTextureStaging.begin(), m_materialTextureStaging.end(),
                                       [&](const RetiredStaging& staging) {
                                           return frame.frameNumber >= staging.frame + kFramesInFlight;
                                       }),
        m_materialTextureStaging.end());

    if (!m_pendingUploads)
        return;
    PendingUploads& uploads = *m_pendingUploads;
    if (uploads.recordedFrame != std::numeric_limits<std::uint64_t>::max())
    {
        if (frame.frameNumber >= uploads.recordedFrame + kFramesInFlight)
            m_pendingUploads.reset();
        return;
    }
    uploads.recordedFrame = frame.frameNumber;
    if (uploads.vertexStaging && m_vertexBuffer)
    {
        cmd.CopyBuffer(*uploads.vertexStaging, *m_vertexBuffer, uploads.vertexStaging->SizeBytes());
        cmd.TransitionBuffer(*m_vertexBuffer, ixrhi::IXRHIBufferState::TransferDst, ixrhi::IXRHIBufferState::VertexRead);
    }
    if (uploads.indexStaging && m_indexBuffer)
    {
        cmd.CopyBuffer(*uploads.indexStaging, *m_indexBuffer, uploads.indexStaging->SizeBytes());
        cmd.TransitionBuffer(*m_indexBuffer, ixrhi::IXRHIBufferState::TransferDst, ixrhi::IXRHIBufferState::IndexRead);
    }
    if (uploads.lodIndexStaging && m_lodIndexBuffer)
    {
        cmd.CopyBuffer(*uploads.lodIndexStaging, *m_lodIndexBuffer, uploads.lodIndexStaging->SizeBytes());
        cmd.TransitionBuffer(*m_lodIndexBuffer, ixrhi::IXRHIBufferState::TransferDst,
            ixrhi::IXRHIBufferState::IndexRead);
    }
    if (uploads.shadowCardVertexStaging && m_shadowCardVertexBuffer)
    {
        cmd.CopyBuffer(*uploads.shadowCardVertexStaging, *m_shadowCardVertexBuffer,
            uploads.shadowCardVertexStaging->SizeBytes());
        cmd.TransitionBuffer(*m_shadowCardVertexBuffer, ixrhi::IXRHIBufferState::TransferDst,
            ixrhi::IXRHIBufferState::VertexRead);
    }
    const std::array<Texture*, 3> textures = {&m_texture, &m_normalTexture, &m_ormTexture};
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        const std::shared_ptr<ixrhi::IXRHIBuffer>& staging = uploads.textureStaging[i];
        if (staging && textures[i]->image)
            RecordTextureUpload(cmd, *textures[i], *staging);
    }
}

bool StaticMeshRenderer::UploadTexture(ixrhi::IXRHIDevice& rhi, const RgbaImage& source, Texture& texture,
                                       std::shared_ptr<ixrhi::IXRHIBuffer>* staging)
{
    const RgbaImage& image = source;
    const ixrhi::IXRHITextureUsage sampledUpload =
        ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    ixrhi::IXRHIFormat format = image.format;
    if (!rhi.IsTextureFormatSupported(format, sampledUpload))
    {
        // A half-float payload must never be reinterpreted as RGBA8.
        if (format == ixrhi::IXRHIFormat::R16G16B16A16Float)
            return false;
        format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;  // the same RGBA8 texels
        if (!rhi.IsTextureFormatSupported(format, sampledUpload))
            return false;
    }

    ixrhi::IXRHITextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = format;
    desc.usage = sampledUpload;
    desc.debugName = "StaticMesh:" + image.name;
    std::shared_ptr<ixrhi::IXRHITexture> uploaded;
    if (staging)
    {
        uploaded = rhi.CreateTexture(desc, nullptr, 0);
        *staging = CreateRhiBuffer(rhi,
            image.pixels.size(),
            ixrhi::IXRHIBufferUsage::TransferSrc,
            ixrhi::IXRHICpuAccess::Write,
            image.pixels.data(),
            ("StaticMesh:" + image.name + ":Staging").c_str());
        if (!*staging)
            return false;
    }
    else
    {
        uploaded = rhi.CreateTexture(desc, image.pixels.data(), image.pixels.size());
    }
    if (!uploaded)
        return false;

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Nearest;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::Repeat;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::Repeat;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::Repeat;
    samplerDesc.maxLod = 1.0f;
    const ixrhi::IXRHICapabilities& caps = rhi.GetCapabilities();
    if (caps.supportsAnisotropy)
        samplerDesc.maxAnisotropy = caps.maxAnisotropy;
    samplerDesc.debugName = "StaticMesh:" + image.name + ":Sampler";
    auto sampler = rhi.CreateSampler(samplerDesc);
    if (!sampler)
        return false;

    texture.image = std::move(uploaded);
    texture.sampler = std::move(sampler);
    texture.name = image.name;
    texture.width = image.width;
    texture.height = image.height;
    texture.mipLevels = 1;
    texture.format = format;
    return true;
}

void StaticMeshRenderer::DecodeTextures(const std::string& modelPath)
{
    RgbaImage& diffuse = m_decodedTextures[0];
    RgbaImage& normal = m_decodedTextures[1];
    RgbaImage& orm = m_decodedTextures[2];
    diffuse = {};
    normal = {};
    orm = {};
    if (!m_assets || !LoadGltfMaterialTextures(*m_assets, modelPath, diffuse, normal, orm))
    {
        LogFormat("[STATIC-MESH] using fallback PBR textures for %s", modelPath.c_str());
    }
    if (diffuse.pixels.empty())
        diffuse = CreateFallbackWhiteImage(modelPath);
    if (normal.pixels.empty())
        normal = CreateFallbackNormalImage(modelPath);
    if (orm.pixels.empty())
        orm = CreateFallbackOrmImage(modelPath);
}

bool StaticMeshRenderer::UploadDecodedTextures(ixrhi::IXRHIDevice& rhi)
{
    const bool defer = m_deferUploads && m_pendingUploads;
    const bool uploaded =
        UploadTexture(rhi, m_decodedTextures[0], m_texture, defer ? &m_pendingUploads->textureStaging[0] : nullptr) &&
        UploadTexture(rhi, m_decodedTextures[1], m_normalTexture, defer ? &m_pendingUploads->textureStaging[1] : nullptr) &&
        UploadTexture(rhi, m_decodedTextures[2], m_ormTexture, defer ? &m_pendingUploads->textureStaging[2] : nullptr);
    for (RgbaImage& image : m_decodedTextures)
        image = {};
    return uploaded;
}

const StaticMeshRenderer::Texture* StaticMeshRenderer::EnsureMaterialTexture(ixrhi::IXRHIDevice& rhi,
    const std::optional<Guid>& guid,
    ixrhi::IXRHIFormat format,
    const char* role)
{
    if (!guid)
        return nullptr;

    const MaterialTextureKey key{MaterialTextureRoleIndex(role), *guid};
    if (const auto it = m_materialTextureCache.find(key); it != m_materialTextureCache.end())
        return it->second.image != nullptr ? &it->second : nullptr;
    if (m_failedMaterialTextureKeys.find(key) != m_failedMaterialTextureKeys.end())
        return nullptr;
    const std::string guidText = guid->toString();

    const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*guid);
    if (!path)
    {
        m_failedMaterialTextureKeys.insert(key);
        LogFormat("[MATBIND-DIAG] texture resolve failed role=%s guid=%s reason=guid_not_found",
            role ? role : "texture",
            guidText.c_str());
        return nullptr;
    }

    // Called while drawing (inside a render pass): the texture is decoded on a loading thread and
    // made and copied in by RecordPendingUploads; until then the model's own texture stands in.
    (void)rhi;
    if (m_materialTextureLoads.find(key) != m_materialTextureLoads.end())
        return nullptr;  // on its way
    auto load = std::make_unique<MaterialTextureLoad>();
    load->path = *path;
    load->format = format;
    load->role = role ? role : "texture";
    MaterialTextureLoad* started = load.get();
    m_materialTextureLoads.emplace(key, std::move(load));
    ixjobs::JobSystem::Instance().SubmitBackground(
        [](void* data, std::uint32_t) {
            MaterialTextureLoad& texture = *static_cast<MaterialTextureLoad*>(data);
            texture.ok = DecodeTextureFile(texture.path, texture.format, texture.role.c_str(), texture.image);
        },
        started, 0, &started->decoded);
    return nullptr;
}

StaticMeshRenderer::MaterialTextureViews StaticMeshRenderer::ResolveMaterialTextureViews(ixrhi::IXRHIDevice& rhi,
    const Instance& instance,
    std::uint32_t materialSlot)
{
    auto materialOf = [](const Texture& texture) {
        MaterialTexture material{};
        material.texture = texture.image;
        material.sampler = texture.sampler;
        return material;
    };

    MaterialTextureViews views{};
    views.baseColor = materialOf(m_texture);
    views.normal = materialOf(m_normalTexture);
    views.orm = materialOf(m_ormTexture);
    if (m_isTreeImpostor && !m_materialDefaults.empty())
    {
        const auto& defaults = m_materialDefaults.front();
        views.alphaMode = defaults.alphaMode;
        views.alphaCutoff = defaults.alphaCutoff;
        views.fragmentShaderAlphaPath = AlphaFragmentPath(views.alphaMode);
        views.unlit = defaults.unlit;
        views.resolvedMaterial = "tree_impostor";
        return views;
    }

    if (materialSlot >= instance.materialSlots.size())
        return views;

    const std::string& slotGuid = instance.materialSlots[materialSlot];
    const std::optional<Guid> materialGuid = Guid::fromString(slotGuid);
    if (!materialGuid)
        return views;

    MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(*materialGuid);
    if (!material)
    {
        views.resolvedMaterial = "pink_missing";
        return views;
    }

    views.resolvedMaterial = material->name.empty()
        ? material->path.filename().generic_string()
        : material->name;
    views.baseColorTextureGuid = material->baseColorTexture;
    views.alphaMode = MaterialAlphaModeName(material->alphaMode);
    views.alphaCutoff = material->alphaCutoff;
    views.fragmentShaderAlphaPath = AlphaFragmentPath(views.alphaMode);
    views.unlit = material->shadingMode == MaterialAsset::ShadingMode::Unlit;

    if (const Texture* texture =
            EnsureMaterialTexture(rhi, material->baseColorTexture, ixrhi::IXRHIFormat::R8G8B8A8Srgb, "baseColor"))
        views.baseColor = materialOf(*texture);
    if (!views.unlit)
    {
        if (const Texture* texture =
                EnsureMaterialTexture(rhi, material->normalTexture, ixrhi::IXRHIFormat::R8G8B8A8Unorm, "normal"))
            views.normal = materialOf(*texture);
        if (const Texture* texture =
                EnsureMaterialTexture(rhi,
                    material->metallicRoughnessTexture,
                    ixrhi::IXRHIFormat::R8G8B8A8Unorm,
                    "metallicRoughness"))
            views.orm = materialOf(*texture);
    }

    return views;
}

void StaticMeshRenderer::UpdateMaterialTextureDescriptors(const BindSlot& slot, const MaterialTextureViews& textures)
{
    if (!slot.page)
        return;

    std::array<BoundSlotTexture, 3>& bound = slot.page->textures[slot.set];
    auto update = [&](std::uint32_t binding, const MaterialTexture& material, BoundSlotTexture& current) {
        if (!material.texture || !material.sampler)
            return;
        if (current.texture == material.texture.get() && current.sampler == material.sampler.get())
            return;  // already bound: skip the descriptor write
        slot.page->group->UpdateTexture(slot.set, binding, material.texture, material.sampler);
        current = {material.texture.get(), material.sampler.get()};
    };
    update(1, textures.baseColor, bound[0]);
    update(2, textures.normal, bound[1]);
    update(3, textures.orm, bound[2]);
}

bool StaticMeshRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    const ixrhi::IXRHIShaderStage allStages =
        ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment;
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, allStages},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {2, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {3, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {4, ixrhi::IXRHIBindingType::StorageBuffer, allStages},
        {5, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},  // sun shadow cascades
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindPages.clear();
    m_loggedBindPagesFull = false;
    if (!AddBindPage(rhi))
        return false;
    m_boundSunShadowTexture = m_sunShadow.sampler ? m_sunShadow.texture.get() : nullptr;
    return true;
}

StaticMeshRenderer::BindPage* StaticMeshRenderer::AddBindPage(ixrhi::IXRHIDevice& rhi)
{
    if (!m_bindLayout || m_bindPages.size() >= kMaxBindPages)
        return nullptr;
    constexpr uint32_t kSets = kFramesInFlight * kUniformSlots;
    auto page = std::make_unique<BindPage>();
    page->group = rhi.CreateBindGroup(*m_bindLayout, kSets);
    page->uniforms = CreateRhiBuffer(rhi,
        kUniformStride * kSets,
        ixrhi::IXRHIBufferUsage::Uniform,
        ixrhi::IXRHICpuAccess::Write,
        nullptr,
        ("StaticMesh:" + m_modelPath + ":UBO").c_str());
    if (!page->group || !page->uniforms)
        return nullptr;
    for (uint32_t set = 0; set < kSets; ++set)
    {
        const uint32_t frame = set / kUniformSlots;
        page->group->UpdateBuffer(set, 0, page->uniforms, kUniformStride * set, sizeof(UniformBlock));
        if (m_texture.image && m_texture.sampler)
            page->group->UpdateTexture(set, 1, m_texture.image, m_texture.sampler);
        if (m_normalTexture.image && m_normalTexture.sampler)
            page->group->UpdateTexture(set, 2, m_normalTexture.image, m_normalTexture.sampler);
        if (m_ormTexture.image && m_ormTexture.sampler)
            page->group->UpdateTexture(set, 3, m_ormTexture.image, m_ormTexture.sampler);
        page->textures[set] = {{{m_texture.image.get(), m_texture.sampler.get()},
            {m_normalTexture.image.get(), m_normalTexture.sampler.get()},
            {m_ormTexture.image.get(), m_ormTexture.sampler.get()}}};
        page->group->UpdateBuffer(set, 4, m_instanceBuffers[frame], 0,
            sizeof(InstanceBlock) * m_instanceBufferCapacity[frame]);
        page->instances[set] = m_instanceBuffers[frame].get();
        if (m_sunShadow.texture && m_sunShadow.sampler)
            page->group->UpdateTexture(set, 5, m_sunShadow.texture, m_sunShadow.sampler);
    }
    m_bindPages.push_back(std::move(page));
    return m_bindPages.back().get();
}

void StaticMeshRenderer::BeginFrameSlots(const ixrhi::IXRHIFrameInfo& frame)
{
    if (m_worldRenderFrameNumber == frame.frameNumber)
        return;
    m_worldRenderFrameNumber = frame.frameNumber;
    m_worldRenderFrameIndex = frame.frameIndex % kFramesInFlight;
    m_worldUniformCursor = 0;
    m_worldInstanceCursor = 0;
}

std::optional<StaticMeshRenderer::BindSlot> StaticMeshRenderer::NextBindSlot(uint32_t frameIndex)
{
    if (!m_rhi || frameIndex >= kFramesInFlight || !m_instanceBuffers[frameIndex])
        return std::nullopt;
    const uint32_t pageIndex = m_worldUniformCursor / kUniformSlots;
    while (pageIndex >= m_bindPages.size())
    {
        if (!AddBindPage(*m_rhi))
        {
            if (!m_loggedBindPagesFull)
            {
                LogFormat("[MESH] static mesh draws skipped: more than %u draws in one frame model=%s",
                    kMaxBindPages * kUniformSlots, m_modelPath.c_str());
                m_loggedBindPagesFull = true;
            }
            return std::nullopt;
        }
    }
    BindSlot slot;
    slot.page = m_bindPages[pageIndex].get();
    slot.set = frameIndex * kUniformSlots + m_worldUniformCursor % kUniformSlots;
    slot.id = pageIndex * kFramesInFlight * kUniformSlots + slot.set;
    ++m_worldUniformCursor;
    // The frame grew the instance buffer since this set last pointed at it. The set is unbound: the
    // frames that bound it have run (this frame index's fence), and this one takes each set once.
    const ixrhi::IXRHIBuffer* instances = m_instanceBuffers[frameIndex].get();
    if (slot.page->instances[slot.set] != instances)
    {
        slot.page->group->UpdateBuffer(slot.set, 4, m_instanceBuffers[frameIndex], 0,
            sizeof(InstanceBlock) * m_instanceBufferCapacity[frameIndex]);
        slot.page->instances[slot.set] = instances;
    }
    return slot;
}

void StaticMeshRenderer::SetSunShadow(const SunShadowReceive& shadow)
{
    m_sunShadow = shadow;
    if (m_bindPages.empty() || !shadow.texture || !shadow.sampler || shadow.texture.get() == m_boundSunShadowTexture)
        return;
    // A new map (first one, or recreated): every set's binding 5. Not while a frame may still use
    // the sets: the map is set up front and changes only with the device's resources.
    for (const std::unique_ptr<BindPage>& page : m_bindPages)
    {
        for (uint32_t set = 0; set < kFramesInFlight * kUniformSlots; ++set)
            page->group->UpdateTexture(set, 5, shadow.texture, shadow.sampler);
    }
    m_boundSunShadowTexture = shadow.texture.get();
}

bool StaticMeshRenderer::EnsureInstanceCapacity(ixrhi::IXRHIDevice& rhi,
    uint32_t frameIndex,
    std::uint32_t requiredRecords)
{
    if (frameIndex >= kFramesInFlight)
        return false;
    requiredRecords = std::max<std::uint32_t>(1u, requiredRecords);
    if (m_instanceBufferCapacity[frameIndex] >= requiredRecords && m_instanceBuffers[frameIndex])
        return true;

    std::uint32_t nextCapacity = std::max<std::uint32_t>(kInitialInstanceCapacity, m_instanceBufferCapacity[frameIndex]);
    while (nextCapacity < requiredRecords)
        nextCapacity *= 2u;

    // The frame's draws recorded so far read their records from the old buffer (their sets keep it
    // alive); the sets taken from now on point at the new one (NextBindSlot), which gets the records
    // appended from here on.
    m_instanceBuffers[frameIndex] = CreateRhiBuffer(rhi,
        sizeof(InstanceBlock) * nextCapacity,
        ixrhi::IXRHIBufferUsage::Storage,
        ixrhi::IXRHICpuAccess::Write,
        nullptr,
        ("StaticMesh:" + m_modelPath + ":Instances").c_str());
    if (!m_instanceBuffers[frameIndex])
        return false;
    m_instanceBufferCapacity[frameIndex] = nextCapacity;
    m_lastInstanceBufferRebuilt = true;
    return true;
}

namespace
{
bool EnvironmentSwitchOn(const char* name);
} // namespace

bool StaticMeshRenderer::CreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_bindLayout)
        return false;

    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto ps = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    auto unlitPs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_unlit_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    // The opaque draws' fragment shaders: no discard, so the depth test runs before shading.
    auto opaquePs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_opaque_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    auto unlitOpaquePs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_unlit_opaque_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    auto outlineVs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_outline_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto outlinePs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_outline_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "PSMain");
    // (The shadow pass's alpha test: the same vertex output, so it pairs with the view's vertex shader.)
    auto maskDepthPs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_shadow_mask_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "ShadowMaskPS");
    if (!vs || !ps || !unlitPs || !opaquePs || !unlitOpaquePs || !outlineVs || !outlinePs || !maskDepthPs)
        return false;

    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = ps;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, normal)},
        {2, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, uv)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::Less;
    desc.blendAttachments = {{false,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::Zero,
        ixrhi::IXRHIBlendOp::Add}};
    desc.sampleCount = 1;
    desc.targetRenderPass = m_targetPass;

    // All 5 variants share vertex input/assembly/viewport/raster(base)/
    // multisample/depth(base)/blend/dynamic state; only the fragment shader
    // (and, for the outline, cull + depth-write/compare) differ — exactly like
    // the pre-migration single VkGraphicsPipelineCreateInfo reuse.
    auto createVariant = [&](const char* name,
                             const std::shared_ptr<ixrhi::IXRHIShader>& fragment,
                             std::unique_ptr<ixrhi::IXRHIGraphicsPipeline>& target) {
        ixrhi::IXRHIGraphicsPipelineDesc variant = desc;
        variant.fragmentShader = fragment;
        variant.debugName = name;
        target = rhi.CreateGraphicsPipeline(variant);
        return target != nullptr;
    };
    desc.debugName = "StaticMesh:Opaque";
    // (IX_OPAQUE_NO_DISCARD=0: the opaque draws with the shader that can discard, as before; for comparing.)
    static const bool opaqueVariants = EnvironmentSwitchOn("IX_OPAQUE_NO_DISCARD");
    if (!createVariant("StaticMesh:Opaque", opaqueVariants ? opaquePs : ps, m_pipeline) ||
        !createVariant("StaticMesh:Mask", ps, m_maskPipeline) ||
        !createVariant("StaticMesh:Unlit", opaqueVariants ? unlitOpaquePs : unlitPs, m_unlitPipeline) ||
        !createVariant("StaticMesh:UnlitMask", unlitPs, m_unlitMaskPipeline))
        return false;

    // The masked draws' two steps: depth only (alpha-tested), then colour on that depth (less-or-equal,
    // not written: equal for the nearest texel, which is the only one left).
    ixrhi::IXRHIGraphicsPipelineDesc maskDepth = desc;
    maskDepth.fragmentShader = maskDepthPs;
    maskDepth.blendAttachments[0].writeColor = false;
    maskDepth.debugName = "StaticMesh:MaskDepth";
    m_maskDepthPipeline = rhi.CreateGraphicsPipeline(maskDepth);
    ixrhi::IXRHIGraphicsPipelineDesc onDepth = desc;
    onDepth.depthWriteEnable = false;
    onDepth.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    onDepth.debugName = "StaticMesh:MaskOnDepth";
    m_maskOnDepthPipeline = rhi.CreateGraphicsPipeline(onDepth);
    onDepth.fragmentShader = unlitPs;
    onDepth.debugName = "StaticMesh:UnlitMaskOnDepth";
    m_unlitMaskOnDepthPipeline = rhi.CreateGraphicsPipeline(onDepth);
    if (!m_maskDepthPipeline || !m_maskOnDepthPipeline || !m_unlitMaskOnDepthPipeline)
        return false;

    // Alpha-blended: over what is drawn, depth tested but not written. Each side of the faces in its
    // own draw, back faces first (outward faces are the front ones, see the outline below), so a
    // closed transparent shape blends its far side under its near side.
    ixrhi::IXRHIGraphicsPipelineDesc blend = desc;
    blend.depthWriteEnable = false;
    blend.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    blend.blendAttachments = {{true,
        ixrhi::IXRHIBlendFactor::SrcAlpha,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add,
        ixrhi::IXRHIBlendFactor::One,
        ixrhi::IXRHIBlendFactor::OneMinusSrcAlpha,
        ixrhi::IXRHIBlendOp::Add}};
    const std::array<std::shared_ptr<ixrhi::IXRHIShader>, 2> blendFragments = {ps, unlitPs};
    const std::array<std::array<const char*, 2>, 2> blendNames = {{
        {"StaticMesh:BlendBack", "StaticMesh:BlendFront"},
        {"StaticMesh:UnlitBlendBack", "StaticMesh:UnlitBlendFront"},
    }};
    for (std::size_t shading = 0; shading < 2; ++shading)
    {
        for (std::size_t side = 0; side < 2; ++side)
        {
            blend.fragmentShader = blendFragments[shading];
            blend.cullMode = side == 0 ? ixrhi::IXRHICullMode::Front : ixrhi::IXRHICullMode::Back;
            blend.debugName = blendNames[shading][side];
            m_blendPipelines[shading][side] = rhi.CreateGraphicsPipeline(blend);
            if (!m_blendPipelines[shading][side])
                return false;
        }
    }

    desc.vertexShader = outlineVs;
    desc.fragmentShader = outlinePs;
    desc.cullMode = ixrhi::IXRHICullMode::Front;
    desc.depthWriteEnable = false;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.debugName = "StaticMesh:Outline";
    m_outlinePipeline = rhi.CreateGraphicsPipeline(desc);
    if (!m_outlinePipeline)
        return false;

    LogFormat("[MATERIAL] static mesh shading pipelines ready lit=%s unlit=%s",
        m_pipeline->DebugName().c_str(),
        m_unlitPipeline->DebugName().c_str());
    return true;
}

void StaticMeshRenderer::RenderInWorld(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds,
    const WorldCamera& camera,
    const Instance& instance,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    const InstanceList instances = {&instance};
    RenderBatchInWorld(cmd, frame, timeSeconds, camera, instances, targetWidth, targetHeight);
}

void StaticMeshRenderer::RenderBatchInWorld(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds,
    const WorldCamera& camera,
    const InstanceList& instances,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    LodConfig defaultLod{};
    RenderLodBatchInWorld(cmd, frame, timeSeconds, camera, instances, defaultLod, 0, 0, targetWidth, targetHeight);
}

namespace
{
// A comparison switch: on unless the environment sets it to 0 (IX_VIEW_LOD, IX_SHADOW_LOD).
bool EnvironmentSwitchOn(const char* name)
{
    std::string value;
#if defined(_WIN32)
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, name) == 0 && text)
    {
        value = text;
        std::free(text);
    }
#else
    if (const char* text = std::getenv(name))
        value = text;
#endif
    return value != "0";
}
} // namespace

bool StaticMeshRenderer::HasTreeImpostor() const
{
    return m_treeImpostor && m_treeImpostor->valid && m_treeImpostor->filesValid && m_treeImpostor->renderer->IsLoaded();
}

bool StaticMeshRenderer::RenderTreeImpostors(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds, const WorldCamera& camera, const InstanceList& instances,
    const LodConfig& config, std::uint64_t configHash, std::uint32_t lodLevel,
    std::uint32_t targetWidth, std::uint32_t targetHeight)
{
    static const bool enabled = EnvironmentSwitchOn("IX_TREE_IMPOSTORS");
    auto* state = m_treeImpostor.get();
    if (!enabled || !state || state->partitioning || !frame.frameActive || !state->renderer->IsLoaded() ||
        !UploadsRecorded() || !state->renderer->UploadsRecorded() || !m_boundSunShadowTexture) return false;
    const auto revision = MaterialAssetManager::Instance().Revision();
    const auto now = std::chrono::steady_clock::now();
    if (now - state->checkedFiles >= std::chrono::seconds(1) && !state->sourceTimes.empty())
    {
        state->checkedFiles = now;
        if (auto root = m_assets->RootPath())
        {
            std::error_code metadataError;
            const auto metadataStamp = std::filesystem::last_write_time(*root / (m_modelPath + ".impostor.json"), metadataError);
            state->filesValid &= !metadataError && metadataStamp == state->metadataTime;
            for (std::size_t i=0;i<state->sourceTimes.size();++i)
            {
                std::error_code error;
                const auto stamp = std::filesystem::last_write_time(*root / state->data.sourcePaths[i], error);
                state->filesValid &= !error && stamp == state->sourceTimes[i];
            }
        }
    }
    if (state->checkedRevision != revision)
    {
        bool valid = true;
        for (std::size_t i=0;i<state->data.materials.size();++i)
        {
            const auto guid = Guid::fromString(state->data.materials[i]);
            const auto* material = guid ? MaterialAssetManager::Instance().getOrLoad(*guid) : nullptr;
            valid &= material && tree_tool::TreeImpostorMaterialSignature(*material) == state->data.materialSignatures[i];
        }
        if (state->valid && !valid) LogFormat("[TREE-IMPOSTOR] material/texture changed; using mesh until rebaked model=%s", m_modelPath.c_str());
        state->valid = valid;
        state->checkedRevision = revision;
    }
    if (!state->valid || !state->filesValid || instances.empty()) return false;
    constexpr float pi = 3.14159265358979323846f;
    state->nearInstances.clear(); state->farInstances.clear();
    std::size_t faded = 0, sprites = 0;
    std::uint32_t impostorTrees = 0;
    // Grow arenas without clearing their per-instance vector capacities each frame.
    if (state->fading.size() < instances.size()) state->fading.resize(instances.size());
    if (state->billboards.size() < instances.size() * 2) state->billboards.resize(instances.size() * 2);
    if (state->billboardPrepared.size() < instances.size() * 2) state->billboardPrepared.resize(instances.size() * 2);
    for (const Instance* source : instances)
    {
        const float scale = source->scale[0];
        bool eligible = !source->selectedForOutline && scale > 0 &&
            std::abs(source->scale[1]-scale) <= scale*0.001f && std::abs(source->scale[2]-scale) <= scale*0.001f &&
            std::abs(source->rotation[0]) < 0.001f && std::abs(source->rotation[2]) < 0.001f &&
            source->materialSlots.size() == state->data.materials.size() &&
            std::equal(source->materialSlots.begin(), source->materialSlots.end(), state->data.materials.begin()) &&
            std::none_of(source->materialOverrides.begin(), source->materialOverrides.end(), [](const auto& entry) { return entry.enabled; });
        const auto center = xm::TransformPoint(BuildStaticMeshModelMatrix(*source),
            {state->data.center[0], state->data.center[1], state->data.center[2]});
        const float dx = static_cast<float>(camera.eye.x) - center.x;
        const float dy = static_cast<float>(camera.eye.y) - center.y;
        const float dz = static_cast<float>(camera.eye.z) - center.z;
        const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        const float elevation = std::atan2(dy, std::sqrt(dx*dx + dz*dz));
        // Outside the captured elevations, tilted/reflected trees and edited materials stay meshes.
        eligible &= std::abs(elevation) <= pi/4;
        const float weight = eligible ? tree_tool::TreeImpostorWeight(distance, state->data.distance * scale,
            state->data.transition * scale) : 0.0f;
        if (weight <= 0)
        {
            state->nearInstances.push_back(source);
            continue;
        }
        ++impostorTrees;
        if (weight < 1)
        {
            auto& copy = state->fading[faded++];
            copy = *source;
            copy.coverageMax = 1-weight;
        }
        const float yaw = std::atan2(dx,dz);
        // Original tree yaw uses row-vector RotationY; positive yaw turns +Z toward -X.
        const auto view = tree_tool::SelectTreeImpostorView(yaw + source->rotation[1], elevation, state->data.azimuths);
        float coverage = 1-weight;
        for (int image = 0; image < 2; ++image)
        {
            const float share = weight * (image == 0 ? 1-view.blend : view.blend);
            if (share <= 0) continue;
            const auto uv = tree_tool::TreeImpostorUv(image == 0 ? view.first : view.second,
                view.row, state->data.azimuths, state->data.resolution);
            auto& prepared = state->billboardPrepared[sprites];
            auto& billboard = state->billboards[sprites++];
            const bool changed = prepared.renderer != state->renderer.get() || prepared.materialRevision != revision ||
                billboard.position.x != center.x || billboard.position.y != center.y || billboard.position.z != center.z ||
                billboard.rotation[0] != elevation || billboard.rotation[1] != -yaw || billboard.scale[0] != scale ||
                billboard.tint != source->tint || billboard.materialOverrides.empty() ||
                billboard.materialOverrides[0].uvTiling[0] != uv[0] || billboard.materialOverrides[0].uvTiling[1] != uv[1] ||
                billboard.materialOverrides[0].uvOffset[0] != uv[2] || billboard.materialOverrides[0].uvOffset[1] != uv[3];
            billboard.entityId = source->entityId;
            billboard.position = {center.x,center.y,center.z};
            billboard.rotation[0] = elevation;
            billboard.rotation[1] = -yaw;
            billboard.rotation[2] = 0;
            for (float& value : billboard.scale) value = scale;
            billboard.tint = source->tint;
            billboard.prepared = nullptr;
            billboard.selectedForOutline = false;
            billboard.coverageMin = coverage;
            coverage += share;
            billboard.coverageMax = image == 1 ? 1.0f : coverage;
            billboard.materialOverrides.resize(1);
            auto& material = billboard.materialOverrides.front();
            material = {};
            material.enabled = true;
            material.uvTiling[0] = uv[0]; material.uvTiling[1] = uv[1];
            material.uvOffset[0] = uv[2]; material.uvOffset[1] = uv[3];
            if (changed) state->renderer->PrepareInstance(billboard, prepared);
            billboard.prepared = &prepared;
            state->farInstances.push_back(&billboard);
        }
    }
    if (state->farInstances.empty()) return false;
    for (std::size_t i = 0; i < faded; ++i) state->nearInstances.push_back(&state->fading[i]);
    state->partitioning = true;
    RenderLodBatchInWorld(cmd, frame, timeSeconds, camera, state->nearInstances, config, configHash, lodLevel, targetWidth, targetHeight);
    state->partitioning = false;
    auto& renderer = *state->renderer;
    renderer.SetLightingState(m_lightingState);
    renderer.SetSunShadow(m_sunShadow);
    renderer.RenderBatchInWorld(cmd, frame, timeSeconds, camera, state->farInstances, targetWidth, targetHeight);
    m_lastSubmittedDrawCalls += renderer.LastSubmittedDrawCalls();
    m_lastSubmittedInstances += renderer.LastSubmittedInstances();
    m_lastSubmittedIndexCount += renderer.LastSubmittedIndexCount();
    m_lastSubmittedTriangles += renderer.LastSubmittedTriangles();
    m_lastInstanceBufferBytes += renderer.LastInstanceBufferBytes();
    m_lastInstanceBufferRebuilt |= renderer.LastInstanceBufferRebuilt();
    m_lastMaterialUniformUpdates += renderer.LastMaterialUniformUpdates();
    m_lastImpostorTrees = impostorTrees;
    return true;
}

void StaticMeshRenderer::RenderLodBatchInWorld(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds,
    const WorldCamera& camera,
    const InstanceList& instancesIn,
    const LodConfig& lodConfig,
    std::uint64_t configHash,
    std::uint32_t lodLevel,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    if (RenderTreeImpostors(cmd, frame, timeSeconds, camera, instancesIn, lodConfig, configHash, lodLevel, targetWidth, targetHeight)) return;
    if (!m_treeImpostor || !m_treeImpostor->partitioning) m_lastImpostorTrees = 0;
    m_lastSubmittedDrawCalls = 0;
    m_lastSubmittedInstances = 0;
    m_lastSubmittedIndexCount = 0;
    m_lastSubmittedTriangles = 0;
    m_lastUsedFullResFallback = false;
    m_lastMaterialUniformUpdates = 0;
    m_lastOverrideActiveDraws = 0;
    m_lastInstanceBufferBytes = 0;
    m_lastInstanceBufferRebuilt = false;
    if (!m_pipeline || !m_unlitPipeline || m_bindPages.empty() || m_indices.empty() || instancesIn.empty() ||
        !frame.frameActive || !UploadsRecorded())
        return;
    if (!m_boundSunShadowTexture)
    {
        // Every lit draw samples the sun shadow map: without one bound the descriptors are incomplete.
        static bool loggedNoShadowMap = false;
        if (!loggedNoShadowMap)
        {
            LogFormat("[MESH] static mesh draws skipped: no sun shadow map set (SetSunShadow) model=%s", m_modelPath.c_str());
            loggedNoShadowMap = true;
        }
        return;
    }
    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    const std::uint32_t diagnosticEntityId = instancesIn.empty() ? 0u : instancesIn.front()->entityId;
    const bool useLodBuffer = configHash != 0 && lodLevel > 0 &&
        EnsureLodBuffers(lodConfig, configHash, diagnosticEntityId);
    const LodIndexBuffer* lodSet = nullptr;
    if (useLodBuffer)
    {
        const auto lodIt = m_lodBuffers.find(configHash);
        if (lodIt != m_lodBuffers.end())
            lodSet = &lodIt->second;
    }
    if (lodSet)
    {
        const std::uint32_t requestedLevel = lodLevel;
        const char* fallbackReason = nullptr;
        if (lodSet->levels == 0)
            fallbackReason = "no-levels";
        else if (!lodSet->buffer)
            fallbackReason = "buffer-not-ready";
        else if (requestedLevel >= lodSet->levels)
            fallbackReason = "no-levels";
        else if (lodSet->triangles[requestedLevel] == 0)
            fallbackReason = "zero-tris";

        if (fallbackReason)
        {
            LogFormat("[LOD-PICK] FALLBACK entity=%u reason=%s drawing=full-res",
                diagnosticEntityId,
                fallbackReason);
            lodSet = nullptr;
            lodLevel = 0;
            m_lastUsedFullResFallback = true;
        }
        else
        {
            lodLevel = requestedLevel;
        }
    }
    else
    {
        if (configHash != 0 && lodLevel > 0)
        {
            LogFormat("[LOD-PICK] FALLBACK entity=%u reason=buffer-not-ready drawing=full-res",
                diagnosticEntityId);
            m_lastUsedFullResFallback = true;
        }
        lodLevel = 0;
    }

    // Each instance's view detail level (a model LOD config aside): how many of the levels' errors,
    // at its scale and nearest depth, project under kViewLodMaxErrorPixels (the view's y scale and
    // depth read from the view-projection: row vectors, clip w the depth). The instances are drawn
    // grouped by level (keeping their order within it), so a level is one run.
    static const bool viewLodsEnabled = EnvironmentSwitchOn("IX_VIEW_LOD");
    InstanceList grouped;
    std::vector<std::uint8_t> viewLevels;
    const InstanceList* drawn = &instancesIn;
    if (!lodSet && viewLodsEnabled && m_lodIndexBuffer && !m_viewLodDraws[0].empty())
    {
        const float* vp = camera.viewProjection.m;
        const float pixelsAtUnitDepth =
            0.5f * static_cast<float>(extentHeight) * std::sqrt(vp[1] * vp[1] + vp[5] * vp[5] + vp[9] * vp[9]);
        std::array<std::uint32_t, kLodLevels + 1> counts{};
        viewLevels.resize(instancesIn.size());
        for (std::size_t i = 0; i < instancesIn.size(); ++i)
        {
            const Instance& instance = *instancesIn[i];
            const float scale = std::max({std::abs(instance.scale[0]), std::abs(instance.scale[1]), std::abs(instance.scale[2])});
            const float depth = static_cast<float>(instance.position.x) * vp[3] + static_cast<float>(instance.position.y) * vp[7] +
                static_cast<float>(instance.position.z) * vp[11] + vp[15] - m_modelRadius * scale;
            std::uint8_t level = 0;
            if (depth > camera.nearPlane)
            {
                const float pixelsPerModelUnit = pixelsAtUnitDepth * scale / depth;
                while (level < kLodLevels && kLodErrors[level] * pixelsPerModelUnit <= kViewLodMaxErrorPixels)
                    ++level;
            }
            viewLevels[i] = level;
            ++counts[level];
        }
        if (counts[0] == instancesIn.size())
        {
            viewLevels.clear();
        }
        else
        {
            std::array<std::uint32_t, kLodLevels + 1> next{};
            for (std::size_t level = 1; level < next.size(); ++level)
                next[level] = next[level - 1] + counts[level - 1];
            grouped.resize(instancesIn.size());
            std::vector<std::uint8_t> groupedLevels(instancesIn.size());
            for (std::size_t i = 0; i < instancesIn.size(); ++i)
            {
                const std::uint32_t at = next[viewLevels[i]]++;
                grouped[at] = instancesIn[i];
                groupedLevels[at] = viewLevels[i];
            }
            viewLevels = std::move(groupedLevels);
            drawn = &grouped;
        }
    }
    const InstanceList& instances = *drawn;
    // Whether a draw (submesh) has view levels: its runs are split by level then.
    const auto levelled = [&](std::uint32_t submesh) {
        if (viewLevels.empty() || submesh >= m_draws.size())
            return false;
        for (const std::vector<MeshDraw>& draws : m_viewLodDraws)
        {
            if (submesh < draws.size() && draws[submesh].indexCount != 0)
                return true;
        }
        return false;
    };

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    BeginFrameSlots(frame);
    const uint32_t instanceBase = m_worldInstanceCursor;

    std::vector<StaticMeshRenderer::InstanceBlock> instanceBlocks;
    std::vector<InstancedDrawCommand> drawCommands;
    const std::size_t drawCountForLevel = lodSet
        ? std::count_if(lodSet->draws.begin(), lodSet->draws.end(), [&](const LodMeshDraw& draw) {
            return draw.firstIndex < lodSet->indices.size();
        })
        : m_draws.size();
    instanceBlocks.reserve(instances.size() * std::max<std::size_t>(1u, drawCountForLevel));
    drawCommands.reserve(drawCountForLevel);
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    auto appendDraw = [&](std::uint32_t firstIndex, std::uint32_t indexCount, std::uint32_t materialSlot, std::uint32_t sourceSubmesh) {
        if (indexCount == 0)
            return;
        {
            InstancedDrawCommand command{};
            command.firstIndex = firstIndex;
            command.indexCount = indexCount;
            command.firstInstance = instanceBase + static_cast<uint32_t>(instanceBlocks.size());
            command.instanceCount = static_cast<uint32_t>(instances.size());
            command.materialSlot = materialSlot;
            command.sourceSubmesh = sourceSubmesh;
            const std::size_t first = instanceBlocks.size();
            instanceBlocks.resize(first + instances.size());
            FillInstanceBlocks(viewProjection, instances, materialSlot, instanceBlocks.data() + first);
            for (const Instance* instance : instances)
            {
                if (instance->materialOverrides.empty())
                    continue;
                const bool overrideActive = std::any_of(instance->materialOverrides.begin(),
                    instance->materialOverrides.end(),
                    [&](const MeshSceneEntity::MaterialOverride& material) {
                        return material.enabled && material.slot == materialSlot;
                    });
                if (overrideActive)
                    ++m_lastOverrideActiveDraws;
            }
            drawCommands.push_back(command);
        }
    };
    if (lodSet)
    {
        const std::size_t drawsPerLevel = m_draws.size();
        const std::size_t begin = static_cast<std::size_t>(lodLevel) * drawsPerLevel;
        const std::size_t end = std::min(begin + drawsPerLevel, lodSet->draws.size());
        for (std::size_t i = begin; i < end; ++i)
        {
            const LodMeshDraw& draw = lodSet->draws[i];
            appendDraw(draw.firstIndex, draw.indexCount, draw.materialSlot, draw.sourceDraw);
        }
    }
    else
    {
        for (std::size_t i = 0; i < m_draws.size(); ++i)
        {
            const MeshDraw& draw = m_draws[i];
            appendDraw(draw.firstIndex, draw.indexCount, draw.materialSlot, static_cast<std::uint32_t>(i));
        }
    }

    if (!AppendInstanceBlocks(frameIndex, instanceBlocks))
        return;
    m_lastInstanceBufferBytes = instanceBlocks.size() * sizeof(InstanceBlock);

    cmd.SetViewport(0.0f,
        0.0f,
        static_cast<float>(extentWidth),
        static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
    const std::shared_ptr<ixrhi::IXRHIBuffer>& boundIndexBuffer =
        lodSet && lodSet->buffer ? lodSet->buffer : m_indexBuffer;
    cmd.SetIndexBuffer(*boundIndexBuffer, 0, /*thirtyTwoBit=*/true);
    const ixrhi::IXRHIBuffer* currentIndexBuffer = boundIndexBuffer.get();
    const auto findInstanceIndex = [&](std::uint32_t entityId) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            if (instances[i]->entityId == entityId)
                return i;
        }
        return std::nullopt;
    };
    const std::optional<std::size_t> diagnosticInstanceIndex =
        configHash != 0 && diagnosticEntityId != 0 ? findInstanceIndex(diagnosticEntityId) : std::nullopt;
    std::optional<std::size_t> refInstanceIndex;
    if (configHash == 0)
    {
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            if (instances[i]->entityId != 3u)
            {
                refInstanceIndex = i;
                break;
            }
        }
    }
    bool loggedMaterialE3 = false;
    bool loggedMaterialRef = false;
    const std::uint64_t frameNumber = frame.frameNumber;
    const auto logDrawDiag = [&](const char* tag,
                                 std::uint32_t entityId,
                                 const InstancedDrawCommand& draw,
                                 std::size_t instanceIndex,
                                 uint32_t bindSlot,
                                 const ixrhi::IXRHIGraphicsPipeline* pipelineForDraw) {
        const std::size_t absoluteInstance = static_cast<std::size_t>(draw.firstInstance) + instanceIndex;
        const std::size_t instanceOffset = absoluteInstance * sizeof(InstanceBlock);
        LogFormat("%s entity=%u submesh=%u frame=%llu pipeline=%s vbuf=0x%llx vbufOffset=0 vbufRange=%zu ibuf=0x%llx ibufOffset=0 indexCount=%u indexType=UINT32 ibuf_first=%u ibuf_vertexOffset=0 instbuf=0x%llx instbufOffset=%zu instCount=%u firstInstance=%u bindSlot=%u pushConst_bytes=<none> pushConst_size=0",
            tag,
            entityId,
            draw.sourceSubmesh,
            static_cast<unsigned long long>(frameNumber),
            pipelineForDraw ? pipelineForDraw->DebugName().c_str() : "<none>",
            RhiObjectId(m_vertexBuffer),
            sizeof(Vertex) * m_vertices.size(),
            RhiObjectId(boundIndexBuffer),
            draw.indexCount,
            draw.firstIndex,
            RhiObjectId(m_instanceBuffers[frameIndex]),
            instanceOffset,
            draw.instanceCount,
            draw.firstInstance,
            bindSlot);
    };
    const auto logMaterialDiag = [&](const char* tag,
                                     const Instance& instance,
                                     const InstanceBlock& block,
                                     uint32_t materialSlot,
                                     std::size_t absoluteInstance,
                                     bool isLodActive) {
        const std::size_t instanceOffset = absoluteInstance * sizeof(InstanceBlock);
        const std::size_t materialOffset = instanceOffset + offsetof(InstanceBlock, materialBaseColor);
        const std::size_t materialSize =
            sizeof(block.materialBaseColor) +
            sizeof(block.materialParams) +
            sizeof(block.materialEmissive) +
            sizeof(block.materialUv);
        const std::size_t transformOffset = instanceOffset + offsetof(InstanceBlock, model);
        const float alphaCutoff = m_alphaModeName == "mask" ? 0.5f : 0.0f;
        LogFormat("%s entity=%u frame=%llu alphaMode=%s materialIndex=%u materialBuffer=0x%llx matOffset=%zu matSize=%zu baseColor=%s normal=%s metallicRoughness=%s baseColorFactor=(%.3f,%.3f,%.3f,%.3f) alphaCutoff=%.3f transformBuffer=0x%llx tfOffset=%zu worldMatrix.row0=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row1=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row2=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row3=(%.3f,%.3f,%.3f,%.3f) isLodActive=%s",
            tag,
            instance.entityId,
            static_cast<unsigned long long>(frameNumber),
            AlphaModeForLog(m_alphaModeName),
            materialSlot,
            RhiObjectId(m_instanceBuffers[frameIndex]),
            materialOffset,
            materialSize,
            m_texture.name.c_str(),
            m_normalTexture.name.c_str(),
            m_ormTexture.name.c_str(),
            block.materialBaseColor[0],
            block.materialBaseColor[1],
            block.materialBaseColor[2],
            block.materialBaseColor[3],
            alphaCutoff,
            RhiObjectId(m_instanceBuffers[frameIndex]),
            transformOffset,
            block.model.m[0], block.model.m[1], block.model.m[2], block.model.m[3],
            block.model.m[4], block.model.m[5], block.model.m[6], block.model.m[7],
            block.model.m[8], block.model.m[9], block.model.m[10], block.model.m[11],
            block.model.m[12], block.model.m[13], block.model.m[14], block.model.m[15],
            isLodActive ? "yes" : "no");
    };
    const auto materialSlotForAbsoluteInstance = [&](std::size_t absoluteInstance) -> std::uint32_t {
        for (const InstancedDrawCommand& command : drawCommands)
        {
            const std::size_t begin = static_cast<std::size_t>(command.firstInstance);
            const std::size_t end = begin + static_cast<std::size_t>(command.instanceCount);
            if (absoluteInstance >= begin && absoluteInstance < end)
                return command.materialSlot;
        }
        return 0u;
    };
    if (configHash != 0 && diagnosticInstanceIndex && drawCommands.size() >= 3)
    {
        std::array<std::size_t, 3> slots{};
        std::array<std::uint32_t, 3> materialSlots{};
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            slots[i] = static_cast<std::size_t>(drawCommands[i].firstInstance) + *diagnosticInstanceIndex;
            materialSlots[i] = materialSlotForAbsoluteInstance(slots[i]);
        }
        auto row3 = [&](std::size_t slot, int column) -> float {
            if (slot >= instanceBlocks.size())
                return 0.0f;
            return instanceBlocks[slot].model.m[12 + column];
        };
        LogFormat("[LOD-INSTBUF-DUMP] entity=%u frame=%llu source=cpu-source-array slot0_worldMatrix_row3=(%.3f,%.3f,%.3f,%.3f) slot1_worldMatrix_row3=(%.3f,%.3f,%.3f,%.3f) slot2_worldMatrix_row3=(%.3f,%.3f,%.3f,%.3f) slot0_materialIndex=%u slot1_materialIndex=%u slot2_materialIndex=%u",
            diagnosticEntityId,
            static_cast<unsigned long long>(frameNumber),
            row3(slots[0], 0), row3(slots[0], 1), row3(slots[0], 2), row3(slots[0], 3),
            row3(slots[1], 0), row3(slots[1], 1), row3(slots[1], 2), row3(slots[1], 3),
            row3(slots[2], 0), row3(slots[2], 1), row3(slots[2], 2), row3(slots[2], 3),
            materialSlots[0],
            materialSlots[1],
            materialSlots[2]);
    }
    // The bindings of this call are written over the previous call's records (reusing their strings'
    // buffers): rebuilt every draw of every frame, fresh strings were allocations per draw.
    std::size_t materialBindingCount = 0;
    const ixrhi::IXRHIGraphicsPipeline* boundPipeline = nullptr;
    // The texture descriptors and the world uniform are bound once per draw call, so one instanced
    // draw may only cover instances that share this slot's material: split each command into runs of
    // consecutive instances with the same material GUID (instances of one material stay one draw).
    const auto fades = [](const Instance& instance) { return instance.coverageMin > 0.0f || instance.coverageMax < 1.0f; };
    const auto materialKey = [](const Instance& instance, std::uint32_t slot) -> std::string_view {
        return slot < instance.materialSlots.size() ? std::string_view(instance.materialSlots[slot]) : std::string_view();
    };
    // Resolved once per run, then drawn by the opaque or the mask pass (resolving inside each pass
    // did the material/texture lookups twice per draw, every frame).
    struct MaterialRun
    {
        const InstancedDrawCommand* command = nullptr;
        InstancedDrawCommand draw;
        const Instance* instance = nullptr;
        MaterialTextureViews textures;
        bool isMask = false;
        bool isBlend = false;  // left to RenderTransparentInWorld
        std::uint8_t level = 0;  // the view detail level its instances are drawn at
    };
    std::vector<MaterialRun> materialRuns;
    materialRuns.reserve(drawCommands.size());
    for (const InstancedDrawCommand& command : drawCommands)
    for (std::uint32_t runStart = 0; runStart < command.instanceCount;)
    {
        const bool byLevel = levelled(command.sourceSubmesh);
        std::uint32_t runEnd = runStart + 1;
        while (runEnd < command.instanceCount &&
               materialKey(*instances[runEnd], command.materialSlot) ==
                   materialKey(*instances[runStart], command.materialSlot) &&
               fades(*instances[runEnd]) == fades(*instances[runStart]) &&
               (!byLevel || viewLevels[runEnd] == viewLevels[runStart]))
            ++runEnd;
        MaterialRun& run = materialRuns.emplace_back();
        run.command = &command;
        run.draw = command;
        run.draw.firstInstance = command.firstInstance + runStart;
        run.draw.instanceCount = runEnd - runStart;
        run.instance = instances[runStart];
        run.textures = ResolveMaterialTextureViews(*m_rhi, *run.instance, run.draw.materialSlot);
        run.isMask = std::strcmp(run.textures.fragmentShaderAlphaPath, "discard") == 0 || fades(*run.instance);
        run.isBlend = std::strcmp(run.textures.fragmentShaderAlphaPath, "blend") == 0;
        run.level = byLevel ? viewLevels[runStart] : 0;
        runStart = runEnd;
    }
    // The masked runs (leaves) in two steps, each run's set made in the first and bound again in the
    // second (MaskStep). The lit shader can discard, which keeps the hardware from rejecting a hidden
    // texel before shading it: a crown seen up close was shaded once for every layer of its leaves.
    // (IX_MASK_PREPASS=0: in one step, as before; for comparing.)
    enum class MaskStep { Single, Depth, Colour };
    static const bool maskPrepass = EnvironmentSwitchOn("IX_MASK_PREPASS");
    std::vector<std::optional<BindSlot>> maskRunSlots(materialRuns.size());
    auto drawPass = [&](bool maskPass, MaskStep step) {
        for (std::size_t runIndex = 0; runIndex < materialRuns.size(); ++runIndex)
        {
            const MaterialRun& run = materialRuns[runIndex];
            if (run.isMask != maskPass || run.isBlend)
                continue;
            const InstancedDrawCommand& command = *run.command;
            const InstancedDrawCommand& draw = run.draw;
            const Instance& runInstance = *run.instance;
            const MaterialTextureViews& materialTextures = run.textures;

            const ixrhi::IXRHIGraphicsPipeline* pipelineForDraw = materialTextures.unlit
                ? (run.isMask ? m_unlitMaskPipeline.get() : m_unlitPipeline.get())
                : (run.isMask ? m_maskPipeline.get() : m_pipeline.get());
            if (step == MaskStep::Depth)
                pipelineForDraw = m_maskDepthPipeline.get();
            else if (step == MaskStep::Colour)
                pipelineForDraw = materialTextures.unlit ? m_unlitMaskOnDepthPipeline.get() : m_maskOnDepthPipeline.get();

            if (boundPipeline != pipelineForDraw)
            {
                cmd.SetGraphicsPipeline(*pipelineForDraw);
                boundPipeline = pipelineForDraw;
            }

            if (step == MaskStep::Colour)
            {
                // The set its depth step made.
                const std::optional<BindSlot>& kept = maskRunSlots[runIndex];
                if (!kept)
                    continue;
                if (currentIndexBuffer != boundIndexBuffer.get())
                {
                    cmd.SetIndexBuffer(*boundIndexBuffer, 0, /*thirtyTwoBit=*/true);
                    currentIndexBuffer = boundIndexBuffer.get();
                }
                cmd.BindGroup(0, *kept->page->group, kept->set);
                cmd.DrawIndexed(draw.indexCount, draw.instanceCount, draw.firstIndex, 0, draw.firstInstance);
                ++m_lastSubmittedDrawCalls;
                m_lastSubmittedIndexCount += draw.indexCount;
                m_lastSubmittedTriangles += static_cast<std::uint64_t>(draw.indexCount/3) * draw.instanceCount;
                continue;
            }
            const std::optional<BindSlot> slot = NextBindSlot(frameIndex);
            if (!slot)
                return;
            UpdateWorldUniform(*slot, camera, runInstance, timeSeconds, draw.materialSlot);
            UpdateMaterialTextureDescriptors(*slot, materialTextures);
            if (step == MaskStep::Depth)
                maskRunSlots[runIndex] = slot;
            const uint32_t bindSlot = slot->id;
            if (materialBindingCount == m_lastMaterialBindings.size())
                m_lastMaterialBindings.emplace_back();
            LastMaterialBinding& binding = m_lastMaterialBindings[materialBindingCount++];
            binding.sourceSubmesh = draw.sourceSubmesh;
            binding.materialSlot = draw.materialSlot;
            binding.bindSlot = bindSlot;
            auto assignName = [](std::string& target, const auto& resource) {
                if (resource)
                    target.assign(resource->DebugName());
                else
                    target.clear();
            };
            assignName(binding.baseColorTexture, materialTextures.baseColor.texture);
            assignName(binding.normalTexture, materialTextures.normal.texture);
            assignName(binding.ormTexture, materialTextures.orm.texture);
            binding.resolvedMaterial = materialTextures.resolvedMaterial;
            binding.baseColorTextureGuid = materialTextures.baseColorTextureGuid;
            binding.alphaMode = materialTextures.alphaMode;
            binding.alphaCutoff = materialTextures.alphaCutoff;
            binding.fragmentShaderAlphaPath = materialTextures.fragmentShaderAlphaPath;
            binding.unlit = materialTextures.unlit;
            assignName(binding.pipelineName, pipelineForDraw);
            binding.boundBeforeDraw = true;
            ++m_lastMaterialUniformUpdates;
            cmd.BindGroup(0, *slot->page->group, slot->set);
            const auto inRun = [&](std::size_t index) {
                const std::size_t first = draw.firstInstance - command.firstInstance;
                return index >= first && index < first + draw.instanceCount;
            };
            if (configHash != 0 && diagnosticInstanceIndex && inRun(*diagnosticInstanceIndex))
            {
                const std::size_t absoluteInstance = static_cast<std::size_t>(command.firstInstance) + *diagnosticInstanceIndex;
                if (absoluteInstance < instanceBlocks.size())
                {
                    if (!loggedMaterialE3)
                    {
                        logMaterialDiag("[LOD-MAT-E3]",
                            *instances[*diagnosticInstanceIndex],
                            instanceBlocks[absoluteInstance],
                            draw.materialSlot,
                            absoluteInstance,
                            true);
                        loggedMaterialE3 = true;
                    }
                    logDrawDiag("[LOD-DRAW-E3]", diagnosticEntityId, command, *diagnosticInstanceIndex, bindSlot, pipelineForDraw);
                }
            }
            if (refInstanceIndex && inRun(*refInstanceIndex))
            {
                const std::size_t absoluteInstance = static_cast<std::size_t>(command.firstInstance) + *refInstanceIndex;
                if (absoluteInstance < instanceBlocks.size())
                {
                    if (!loggedMaterialRef)
                    {
                        logMaterialDiag("[LOD-MAT-REF]",
                            *instances[*refInstanceIndex],
                            instanceBlocks[absoluteInstance],
                            draw.materialSlot,
                            absoluteInstance,
                            false);
                        loggedMaterialRef = true;
                    }
                    logDrawDiag("[LOD-DRAW-REF]", instances[*refInstanceIndex]->entityId, command, *refInstanceIndex, bindSlot, pipelineForDraw);
                }
            }
            // An opaque run at its level's indices (a masked one keeps its own: its uvs).
            const MeshDraw* lod = nullptr;
            if (run.level > 0 && !run.isMask)
            {
                const MeshDraw& span = m_viewLodDraws[run.level - 1u][draw.sourceSubmesh];
                if (span.indexCount != 0)
                    lod = &span;
            }
            const ixrhi::IXRHIBuffer* indexBuffer = lod ? m_lodIndexBuffer.get() : boundIndexBuffer.get();
            if (currentIndexBuffer != indexBuffer)
            {
                cmd.SetIndexBuffer(*indexBuffer, 0, /*thirtyTwoBit=*/true);
                currentIndexBuffer = indexBuffer;
            }
            const std::uint32_t indexCount = lod ? lod->indexCount : draw.indexCount;
            cmd.DrawIndexed(indexCount, draw.instanceCount, lod ? lod->firstIndex : draw.firstIndex, 0, draw.firstInstance);
            ++m_lastSubmittedDrawCalls;
            m_lastSubmittedIndexCount += indexCount;
            if (step != MaskStep::Depth)
                m_lastSubmittedTriangles += static_cast<std::uint64_t>(indexCount/3) * draw.instanceCount;
        }
    };
    drawPass(false, MaskStep::Single);
    if (maskPrepass && m_maskDepthPipeline && m_maskOnDepthPipeline && m_unlitMaskOnDepthPipeline)
    {
        drawPass(true, MaskStep::Depth);
        drawPass(true, MaskStep::Colour);
    }
    else
    {
        drawPass(true, MaskStep::Single);
    }
    m_lastMaterialBindings.resize(materialBindingCount);
    if (m_outlinePipeline)
    {
        std::vector<std::uint32_t> outlinedInstances;
        outlinedInstances.reserve(instances.size());
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            if (instances[i]->selectedForOutline)
                outlinedInstances.push_back(i);
        }
        const std::optional<BindSlot> slot = outlinedInstances.empty() ? std::nullopt : NextBindSlot(frameIndex);
        if (slot)
        {
            if (currentIndexBuffer != boundIndexBuffer.get())
                cmd.SetIndexBuffer(*boundIndexBuffer, 0, /*thirtyTwoBit=*/true);
            cmd.SetGraphicsPipeline(*m_outlinePipeline);
            const MaterialTextureViews materialTextures =
                ResolveMaterialTextureViews(*m_rhi, *instances.front(), 0);
            UpdateWorldUniform(*slot, camera, *instances.front(), timeSeconds, 0);
            UpdateMaterialTextureDescriptors(*slot, materialTextures);
            cmd.BindGroup(0, *slot->page->group, slot->set);

            for (const InstancedDrawCommand& draw : drawCommands)
            {
                for (std::uint32_t instanceIndex : outlinedInstances)
                {
                    if (instanceIndex >= draw.instanceCount)
                        continue;
                    cmd.DrawIndexed(draw.indexCount,
                        1,
                        draw.firstIndex,
                        0,
                        draw.firstInstance + instanceIndex);
                }
            }
        }
    }
    m_lastSubmittedInstances = static_cast<std::uint32_t>(instances.size());
}

bool StaticMeshRenderer::PrepareInstance(const Instance& instance, PreparedInstance& out) const
{
    const Mat4 identity = Identity();
    const std::uint32_t slotCount = MaterialSlotCount();
    bool changed = out.renderer != this || out.slots.size() != slotCount;
    out.renderer = this;
    out.slots.resize(slotCount);
    for (std::uint32_t slot = 0; slot < slotCount; ++slot)
    {
        InstanceBlock block{};
        FillStaticMeshInstanceBlock(identity, instance, slot, m_materialDefaults, block, m_isTreeImpostor);
        changed = changed || std::memcmp(&block, &out.slots[slot], sizeof(InstanceBlock)) != 0;
        out.slots[slot] = block;
    }
    bool hasTransparentDraws = false;
    for (const MeshDraw& draw : m_draws)
    {
        if (draw.indexCount != 0 && IsBlendMaterialSlot(instance, draw.materialSlot))
        {
            hasTransparentDraws = true;
            break;
        }
    }
    changed = changed || hasTransparentDraws != out.hasTransparentDraws;
    out.hasTransparentDraws = hasTransparentDraws;
    out.materialRevision = MaterialAssetManager::Instance().Revision();
    return changed;
}

void StaticMeshRenderer::FillInstanceBlock(const WorldMat4& viewProjection,
    const Instance& instance,
    std::uint32_t materialSlot,
    InstanceBlock& out) const
{
    const PreparedInstance* prepared = instance.prepared;
    if (prepared && prepared->renderer == this && materialSlot < prepared->slots.size() &&
        prepared->materialRevision == MaterialAssetManager::Instance().Revision())
    {
        out = prepared->slots[materialSlot];
        out.mvp = Multiply(out.model, viewProjection);
        std::memcpy(out.tint, instance.tint.data(), sizeof(out.tint));
        out.materialAlpha[2] = instance.coverageMin;
        out.materialAlpha[3] = instance.coverageMax;
        return;
    }
    FillStaticMeshInstanceBlock(viewProjection, instance, materialSlot, m_materialDefaults, out, m_isTreeImpostor);
}

bool StaticMeshRenderer::PreparedUsable(const Instance& instance) const
{
    const PreparedInstance* prepared = instance.prepared;
    return prepared && prepared->renderer == this &&
        prepared->materialRevision == MaterialAssetManager::Instance().Revision();
}

void StaticMeshRenderer::FillInstanceBlocks(const WorldMat4& viewProjection,
    const InstanceList& instances,
    std::uint32_t materialSlot,
    InstanceBlock* out) const
{
    const std::uint32_t count = static_cast<std::uint32_t>(instances.size());
    constexpr std::uint32_t kGrain = 1024;
    static const bool parallel = ixjobs::FeatureEnabled("IX_PARALLEL_CULL");
    if (!parallel || count < 2u * kGrain)
    {
        for (std::uint32_t i = 0; i < count; ++i)
            FillInstanceBlock(viewProjection, *instances[i], materialSlot, out[i]);
        return;
    }
    std::vector<std::uint8_t> left(count, 0);  // 1: not prepared, filled below
    ixjobs::JobSystem::Instance().ParallelFor(count, kGrain, [&](std::uint32_t begin, std::uint32_t end, std::uint32_t) {
        for (std::uint32_t i = begin; i < end; ++i)
        {
            if (PreparedUsable(*instances[i]) && materialSlot < instances[i]->prepared->slots.size())
                FillInstanceBlock(viewProjection, *instances[i], materialSlot, out[i]);
            else
                left[i] = 1;
        }
    });
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (left[i])
            FillInstanceBlock(viewProjection, *instances[i], materialSlot, out[i]);
    }
}

bool StaticMeshRenderer::HasTransparentDraws(const Instance& instance) const
{
    const PreparedInstance* prepared = instance.prepared;
    if (prepared && prepared->renderer == this &&
        prepared->materialRevision == MaterialAssetManager::Instance().Revision())
        return prepared->hasTransparentDraws;
    for (const MeshDraw& draw : m_draws)
    {
        if (draw.indexCount != 0 && IsBlendMaterialSlot(instance, draw.materialSlot))
            return true;
    }
    return false;
}

void StaticMeshRenderer::RenderTransparentInWorld(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds,
    const WorldCamera& camera,
    const Instance& instance,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    if (!m_rhi || m_bindPages.empty() || !m_vertexBuffer || !m_indexBuffer || m_indices.empty() || !frame.frameActive ||
        !m_boundSunShadowTexture || !m_blendPipelines[0][0] || !m_blendPipelines[1][1] || !UploadsRecorded())
        return;
    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    std::vector<std::size_t> blendDraws;
    for (std::size_t i = 0; i < m_draws.size(); ++i)
    {
        if (m_draws[i].indexCount != 0 && IsBlendMaterialSlot(instance, m_draws[i].materialSlot))
            blendDraws.push_back(i);
    }
    if (blendDraws.empty())
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    BeginFrameSlots(frame);
    std::vector<InstanceBlock> blocks(blendDraws.size());
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    for (std::size_t i = 0; i < blendDraws.size(); ++i)
        FillInstanceBlock(viewProjection, instance, m_draws[blendDraws[i]].materialSlot, blocks[i]);
    const std::optional<std::uint32_t> base = AppendInstanceBlocks(frameIndex, blocks);
    if (!base)
        return;

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetVertexBuffer(0, *m_vertexBuffer, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
    for (std::size_t i = 0; i < blendDraws.size(); ++i)
    {
        const MeshDraw& draw = m_draws[blendDraws[i]];
        const MaterialTextureViews textures = ResolveMaterialTextureViews(*m_rhi, instance, draw.materialSlot);
        const std::optional<BindSlot> slot = NextBindSlot(frameIndex);
        if (!slot)
            return;
        UpdateWorldUniform(*slot, camera, instance, timeSeconds, draw.materialSlot);
        UpdateMaterialTextureDescriptors(*slot, textures);
        // The set binds against the bound pipeline's layout: this renderer's first (the previous draw
        // may be another renderer's); the second side shares the layout, so the set stays bound.
        const auto& sides = m_blendPipelines[textures.unlit ? 1 : 0];
        cmd.SetGraphicsPipeline(*sides[0]);  // back faces
        cmd.BindGroup(0, *slot->page->group, slot->set);
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, *base + static_cast<std::uint32_t>(i));
        cmd.SetGraphicsPipeline(*sides[1]);  // front faces
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, *base + static_cast<std::uint32_t>(i));
    }
}

std::optional<std::uint32_t> StaticMeshRenderer::AppendInstanceBlocks(uint32_t frameIndex,
    const std::vector<InstanceBlock>& blocks)
{
    const std::uint32_t base = m_worldInstanceCursor;
    const std::uint32_t count = static_cast<std::uint32_t>(blocks.size());
    if (blocks.empty() || !m_rhi || !EnsureInstanceCapacity(*m_rhi, frameIndex, base + count))
        return std::nullopt;
    const std::size_t bytes = blocks.size() * sizeof(InstanceBlock);
    // Appended at the per-frame cursor (not offset 0): this renderer is drawn several times a frame
    // (shadow cascades, Scene and Game views) and each draw keeps its records until the GPU reads them.
    m_instanceBuffers[frameIndex]->Write(static_cast<std::uint64_t>(base) * sizeof(InstanceBlock), blocks.data(), bytes);
    m_worldInstanceCursor = base + count;
    return base;
}

bool StaticMeshRenderer::CreateShadowPipelines(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* shadowPass)
{
    if (!m_assets || !m_bindLayout || !shadowPass)
        return false;
    auto vs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_shadow_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex, "VSMain");
    auto maskPs = LoadShader(rhi, *m_assets, "assets/shaders/static_mesh_shadow_mask_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment, "ShadowMaskPS");
    if (!vs || !maskPs)
        return false;

    // Depth only, like the terrain's cascades: no culling (meshes may be open), a slope-scaled bias.
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = nullptr;
    desc.bindGroupLayouts = {m_bindLayout.get()};
    desc.vertexBindings = {{0, sizeof(Vertex)}};
    desc.vertexAttributes = {
        {0, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, position)},
        {1, 0, ixrhi::IXRHIFormat::R32G32B32Float, offsetof(Vertex, normal)},
        {2, 0, ixrhi::IXRHIFormat::R32G32Float, offsetof(Vertex, uv)},
    };
    desc.topology = ixrhi::IXRHIPrimitiveTopology::TriangleList;
    desc.cullMode = ixrhi::IXRHICullMode::None;
    desc.frontFace = ixrhi::IXRHIFrontFace::Clockwise;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.depthCompareOp = ixrhi::IXRHICompareOp::LessOrEqual;
    desc.depthBias.enable = true;
    desc.depthBias.constantFactor = 1.25f;
    desc.depthBias.slopeFactor = 1.75f;
    desc.sampleCount = 1;
    desc.targetRenderPass = shadowPass;
    desc.debugName = "StaticMesh:Shadow";
    m_shadowPipeline = rhi.CreateGraphicsPipeline(desc);
    desc.fragmentShader = maskPs;
    desc.debugName = "StaticMesh:ShadowMask";
    m_shadowMaskPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_shadowPipeline && m_shadowMaskPipeline;
}

namespace
{
// IX_SHADOW_LOD=0: every caster draws its own triangles into every cascade (for comparing).
bool ShadowLodsEnabled()
{
    return EnvironmentSwitchOn("IX_SHADOW_LOD");
}
} // namespace

void StaticMeshRenderer::RenderShadowCasters(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    const WorldMat4& lightViewProj,
    const InstanceList& instances,
    const ixrhi::IXRHIRenderPass* shadowPass,
    float shadowTexelMeters)
{
    if (!m_rhi || m_bindPages.empty() || !m_vertexBuffer || !m_indexBuffer || m_indices.empty() || m_draws.empty() ||
        instances.empty() || !shadowPass || !frame.frameActive || !m_boundSunShadowTexture || !UploadsRecorded())
        return;
    if (m_shadowPass != shadowPass)
    {
        m_shadowPipeline.reset();
        m_shadowMaskPipeline.reset();
        m_shadowPipelinesFailed = false;
        m_shadowPass = shadowPass;
    }
    if (!m_shadowPipeline && !m_shadowPipelinesFailed && !CreateShadowPipelines(*m_rhi, shadowPass))
    {
        m_shadowPipelinesFailed = true;
        LogFormat("[MESH] static mesh sun shadow pipelines could not be created model=%s", m_modelPath.c_str());
    }
    if (!m_shadowPipeline || !m_shadowMaskPipeline)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    BeginFrameSlots(frame);

    // One record per draw (submesh) and instance, its mvp = model x the cascade's light
    // view-projection; its material says whether it is alpha-masked.
    std::vector<InstanceBlock> blocks(m_draws.size() * instances.size());
    for (std::size_t drawIndex = 0; drawIndex < m_draws.size(); ++drawIndex)
        FillInstanceBlocks(lightViewProj, instances, m_draws[drawIndex].materialSlot,
            blocks.data() + drawIndex * instances.size());
    const std::optional<std::uint32_t> base = AppendInstanceBlocks(frameIndex, blocks);
    if (!base)
        return;
    // Each instance's shadow detail level here: how many of them stay within half a texel at its
    // scale (0: its own triangles; the last: its leaf cards thinned too).
    static const bool lodsEnabled = ShadowLodsEnabled();
    std::vector<std::uint8_t> levels(instances.size(), 0);
    if (lodsEnabled && m_lodIndexBuffer && shadowTexelMeters > 0.0f)
    {
        const float allowed = 0.5f * shadowTexelMeters;
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            const float* scale = instances[i]->scale;
            const float largest = std::max({std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2])});
            std::uint8_t level = 0;
            while (level < kLodLevels && kLodErrors[level] * largest <= allowed)
                ++level;
            levels[i] = level;
        }
    }

    const ixrhi::IXRHIBuffer* boundVertexBuffer = nullptr;
    const ixrhi::IXRHIBuffer* boundIndexBuffer = nullptr;
    const auto thinCards = [&](std::uint32_t i) { return levels[i] == kLodLevels; };
    // Opaque runs need no textures: one set of its own this frame (instances + shadow map bound),
    // taken by the first of them.
    std::optional<BindSlot> opaqueSlot;
    const auto materialKey = [](const Instance& instance, std::uint32_t slot) -> std::string_view {
        return slot < instance.materialSlots.size() ? std::string_view(instance.materialSlots[slot]) : std::string_view();
    };
    const ixrhi::IXRHIGraphicsPipeline* boundPipeline = nullptr;
    const std::uint32_t instanceCount = static_cast<std::uint32_t>(instances.size());
    for (std::size_t drawIndex = 0; drawIndex < m_draws.size(); ++drawIndex)
    {
        const MeshDraw& draw = m_draws[drawIndex];
        if (draw.indexCount == 0)
            continue;
        const std::uint32_t drawBase = *base + static_cast<std::uint32_t>(drawIndex) * instanceCount;
        for (std::uint32_t runStart = 0; runStart < instanceCount;)
        {
            // Alpha-masked and alpha-blended materials (mode 1, 2) cast where their alpha reaches the
            // cutoff: a faint glass casts nothing instead of a solid block.
            const bool masked = blocks[drawBase - *base + runStart].materialAlpha[0] >= 1.0f;
            std::uint32_t runEnd = runStart + 1;
            // Opaque instances share one draw, and masked ones of one material one with its texture.
            if (masked)
            {
                const std::string_view material = materialKey(*instances[runStart], draw.materialSlot);
                while (runEnd < instanceCount && blocks[drawBase - *base + runEnd].materialAlpha[0] >= 1.0f &&
                       materialKey(*instances[runEnd], draw.materialSlot) == material &&
                       thinCards(runEnd) == thinCards(runStart))
                    ++runEnd;
            }
            else
            {
                while (runEnd < instanceCount && blocks[drawBase - *base + runEnd].materialAlpha[0] < 1.0f &&
                       levels[runEnd] == levels[runStart])
                    ++runEnd;
            }
            std::optional<BindSlot> slot;
            if (masked)
            {
                slot = NextBindSlot(frameIndex);
                if (slot)
                    UpdateMaterialTextureDescriptors(*slot,
                        ResolveMaterialTextureViews(*m_rhi, *instances[runStart], draw.materialSlot));
            }
            else
            {
                if (!opaqueSlot)
                    opaqueSlot = NextBindSlot(frameIndex);
                slot = opaqueSlot;
            }
            if (!slot)
                return;
            const ixrhi::IXRHIGraphicsPipeline* pipeline = masked ? m_shadowMaskPipeline.get() : m_shadowPipeline.get();
            if (boundPipeline != pipeline)
            {
                cmd.SetGraphicsPipeline(*pipeline);
                boundPipeline = pipeline;
            }
            cmd.BindGroup(0, *slot->page->group, slot->set);
            // Opaque runs at their detail level; masked ones as they are, or as thinned cards.
            const MeshDraw* lod = nullptr;
            bool cards = false;
            if (masked)
            {
                cards = thinCards(runStart) && m_shadowCardVertexBuffer && !m_shadowCardDraws.empty() &&
                    m_shadowCardDraws[drawIndex].indexCount != 0;
                if (cards)
                    lod = &m_shadowCardDraws[drawIndex];
            }
            else if (levels[runStart] > 0 && !m_shadowLodDraws[levels[runStart] - 1u].empty() &&
                     m_shadowLodDraws[levels[runStart] - 1u][drawIndex].indexCount != 0)
            {
                lod = &m_shadowLodDraws[levels[runStart] - 1u][drawIndex];
            }
            const ixrhi::IXRHIBuffer* vertexBuffer = cards ? m_shadowCardVertexBuffer.get() : m_vertexBuffer.get();
            if (boundVertexBuffer != vertexBuffer)
            {
                cmd.SetVertexBuffer(0, *vertexBuffer, 0);
                boundVertexBuffer = vertexBuffer;
            }
            const ixrhi::IXRHIBuffer* indexBuffer = lod ? m_lodIndexBuffer.get() : m_indexBuffer.get();
            if (boundIndexBuffer != indexBuffer)
            {
                cmd.SetIndexBuffer(*indexBuffer, 0, /*thirtyTwoBit=*/true);
                boundIndexBuffer = indexBuffer;
            }
            const MeshDraw& drawn = lod ? *lod : draw;
            cmd.DrawIndexed(drawn.indexCount, runEnd - runStart, drawn.firstIndex, 0, drawBase + runStart);
            runStart = runEnd;
        }
    }
}

void StaticMeshRenderer::DestroyPipeline()
{
    // RAII release (pipelines own VkPipeline + layout in the backend).
    m_pipeline.reset();
    m_maskPipeline.reset();
    m_unlitPipeline.reset();
    m_unlitMaskPipeline.reset();
    m_maskDepthPipeline.reset();
    m_maskOnDepthPipeline.reset();
    m_unlitMaskOnDepthPipeline.reset();
    m_outlinePipeline.reset();
    for (auto& sides : m_blendPipelines)
    {
        for (auto& pipeline : sides)
            pipeline.reset();
    }
    m_shadowPipeline.reset();
    m_shadowMaskPipeline.reset();
    m_shadowPass = nullptr;
    m_shadowPipelinesFailed = false;
}

void StaticMeshRenderer::Destroy()
{
    StopLodWorker();
    m_treeImpostor.reset();
    m_lastImpostorTrees = 0;
    if (!m_rhi)
        return;
    DestroyPipeline();
    m_bindPages.clear();
    m_bindLayout.reset();
    m_pendingUploads.reset();
    m_vertexBuffer.reset();
    m_indexBuffer.reset();
    m_lodIndexBuffer.reset();
    m_shadowCardVertexBuffer.reset();
    m_lodBuffers.clear();
    // Dropping in-flight uploads waits for their fences and releases staging
    // (parity with the old fence-wait in Destroy).
    m_pendingLodUploads.clear();
    m_retiredLodBuffers.clear();
    for (auto& buffer : m_instanceBuffers)
        buffer.reset();
    m_instanceBufferCapacity = {};
    m_worldRenderFrameNumber = std::numeric_limits<std::uint64_t>::max();
    m_worldRenderFrameIndex = std::numeric_limits<uint32_t>::max();
    m_texture = {};
    m_normalTexture = {};
    m_ormTexture = {};
    m_materialTextureLoads.clear();  // (waits for their decodes)
    m_materialTextureStaging.clear();
    m_materialTextureCache.clear();
    m_failedMaterialTextureKeys.clear();
    // The shadow map belongs to the terrain renderer; holding it past here leaks it at device teardown.
    m_sunShadow = {};
    m_boundSunShadowTexture = nullptr;
    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_boundsMin = {0.0f, 0.0f, 0.0f};
    m_boundsMax = {0.0f, 0.0f, 0.0f};
    m_lastSubmittedDrawCalls = 0;
    m_lastSubmittedInstances = 0;
    m_lastSubmittedIndexCount = 0;
    m_lastUsedFullResFallback = false;
    m_lastMaterialUniformUpdates = 0;
    m_lastOverrideActiveDraws = 0;
    m_lastInstanceBufferBytes = 0;
    m_lastInstanceBufferRebuilt = false;
    m_assets = nullptr;
    m_modelPath.clear();
    m_status = LoadStatus::NotLoaded;
    m_targetPass = nullptr;
    m_rhi = nullptr;
}

void StaticMeshRenderer::UpdateWorldUniform(const BindSlot& slot,
    const WorldCamera& camera,
    const Instance& instance,
    double,
    uint32_t materialSlot)
{
    const Mat4 model = Multiply(
        Multiply(
            Multiply(
                Multiply(Scale(instance.scale[0], instance.scale[1], instance.scale[2]),
                    RotationX(instance.rotation[0])),
                RotationY(instance.rotation[1])),
            RotationZ(instance.rotation[2])),
        Translation(instance.position.x, instance.position.y, instance.position.z));
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    const Mat4 mvp = Multiply(model, viewProjection);

    UniformBlock uniform{mvp, model,
        {instance.tint[0], instance.tint[1], instance.tint[2], instance.tint[3]}};
    const MaterialDefaults defaults = ResolveMaterialSlotDefaults(instance, materialSlot, m_materialDefaults, m_isTreeImpostor);
    std::memcpy(uniform.materialBaseColor, defaults.baseColor, sizeof(uniform.materialBaseColor));
    uniform.materialParams[0] = defaults.metallic;
    uniform.materialParams[1] = defaults.roughness;
    uniform.materialParams[2] = defaults.normalStrength;
    uniform.materialParams[3] = defaults.aoStrength;
    uniform.materialEmissive[0] = defaults.emissive[0];
    uniform.materialEmissive[1] = defaults.emissive[1];
    uniform.materialEmissive[2] = defaults.emissive[2];
    uniform.materialEmissive[3] = 1.0f;
    uniform.materialAlpha[0] = AlphaModeCode(defaults.alphaMode);
    uniform.materialAlpha[1] = std::clamp(defaults.alphaCutoff, 0.0f, 1.0f);
    uniform.materialAlpha[2] = instance.coverageMin;
    uniform.materialUv[0] = defaults.uvTiling[0];
    uniform.materialUv[1] = defaults.uvTiling[1];
    uniform.materialUv[2] = defaults.uvOffset[0];
    uniform.materialUv[3] = defaults.uvOffset[1];
    uniform.materialAlpha[3] = instance.coverageMax;
    for (const MeshSceneEntity::MaterialOverride& overrideSlot : instance.materialOverrides)
    {
        if (overrideSlot.slot != materialSlot || !overrideSlot.enabled)
            continue;
        std::memcpy(uniform.materialBaseColor, overrideSlot.baseColor, sizeof(uniform.materialBaseColor));
        uniform.materialParams[0] = overrideSlot.metallic;
        uniform.materialParams[1] = overrideSlot.roughness;
        uniform.materialParams[2] = overrideSlot.normalStrength;
        uniform.materialParams[3] = overrideSlot.aoStrength;
        uniform.materialEmissive[0] = overrideSlot.emissive[0];
        uniform.materialEmissive[1] = overrideSlot.emissive[1];
        uniform.materialEmissive[2] = overrideSlot.emissive[2];
        uniform.materialEmissive[3] = overrideSlot.emissiveIntensity;
        uniform.materialUv[0] = overrideSlot.uvTiling[0];
        uniform.materialUv[1] = overrideSlot.uvTiling[1];
        uniform.materialUv[2] = overrideSlot.uvOffset[0];
        uniform.materialUv[3] = overrideSlot.uvOffset[1];
        break;
    }
    uniform.cameraPosition[0] = static_cast<float>(camera.eye.x);
    uniform.cameraPosition[1] = static_cast<float>(camera.eye.y);
    uniform.cameraPosition[2] = static_cast<float>(camera.eye.z);
    uniform.cameraPosition[3] = 1.0f;
    FillLightingUniform(m_lightingState, uniform);
    uniform.waterParams[0] = 0.0f;
    uniform.causticParams[0] = 0.0f;
    FillSunShadowUniform(m_sunShadow, uniform);

    if (slot.page && slot.page->uniforms)
        slot.page->uniforms->Write(kUniformStride * slot.set, &uniform, sizeof(uniform));
}
