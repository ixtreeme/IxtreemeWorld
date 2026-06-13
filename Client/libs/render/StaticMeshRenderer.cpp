#include "StaticMeshRenderer.h"

#include "Debug.h"
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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{
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

const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    default: return "UNKNOWN_VK_RESULT";
    }
}

void CheckVk(VkResult result, const char* call, const char* file, int line)
{
    if (result == VK_SUCCESS)
        return;
    LogFormat("%s:%d: Vulkan call failed: %s -> %s (%d)", file, line, call, VkResultName(result), result);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

struct Mat4
{
    float m[16];
};

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
};

struct StaticMeshInstanceBlock
{
    Mat4 mvp;
    Mat4 model;
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float materialBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float materialParams[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float materialEmissive[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float materialUv[4] = {1.0f, 1.0f, 0.0f, 0.0f};
};

struct InstancedDrawCommand
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t materialSlot = 0;
    uint32_t sourceSubmesh = 0;
};

template <typename Handle>
unsigned long long VkHandleValue(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle));
    else
        return static_cast<unsigned long long>(handle);
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

struct RgbaImage
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint8_t> pixels;
};

Mat4 Identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
        }
    }
    return r;
}

Mat4 Scale(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
}

Mat4 RotationX(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Identity();
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

Mat4 RotationY(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Identity();
    r.m[0] = c;
    r.m[2] = s;
    r.m[8] = -s;
    r.m[10] = c;
    return r;
}

Mat4 RotationZ(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Identity();
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

Mat4 Translation(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
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
    const float azimuthRadians = std::clamp(directional.azimuthDegrees, 0.0f, 360.0f) * 3.1415926535f / 180.0f;
    const float elevationRadians = std::clamp(directional.elevationDegrees, 0.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float cosElevation = std::cos(elevationRadians);
    const float sunIntensity = std::max(0.0f, directional.intensity) * (directional.enabled ? 1.0f : 0.0f);
    const float ambientIntensity = std::max(0.0f, ambient.intensity);
    uniform.sunDir[0] = cosElevation * std::sin(azimuthRadians);
    uniform.sunDir[1] = std::sin(elevationRadians);
    uniform.sunDir[2] = cosElevation * std::cos(azimuthRadians);
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
        const float pitch = spot.rotation[0];
        const float yaw = spot.rotation[1];
        const float cosPitch = std::cos(pitch);
        auto& out = uniform.spotLights[i];
        out.position[0] = spot.position[0];
        out.position[1] = spot.position[1];
        out.position[2] = spot.position[2];
        out.position[3] = std::max(0.1f, spot.radius);
        out.direction[0] = std::sin(yaw) * cosPitch;
        out.direction[1] = std::sin(pitch);
        out.direction[2] = std::cos(yaw) * cosPitch;
        out.direction[3] = std::cos(spot.innerConeDegrees * 3.1415926535f / 180.0f);
        const float intensity = spot.enabled ? std::max(0.0f, spot.intensity) : 0.0f;
        out.color[0] = std::max(0.0f, spot.r) * intensity;
        out.color[1] = std::max(0.0f, spot.g) * intensity;
        out.color[2] = std::max(0.0f, spot.b) * intensity;
        out.color[3] = std::cos(spot.outerConeDegrees * 3.1415926535f / 180.0f);
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

void FillStaticMeshInstanceBlock(const WorldCamera& camera,
    const StaticMeshRenderer::Instance& instance,
    uint32_t materialSlot,
    const std::vector<StaticMeshRenderer::MaterialDefaults>& materialDefaults,
    StaticMeshInstanceBlock& out)
{
    out = {};
    out.model = BuildStaticMeshModelMatrix(instance);
    out.mvp = Multiply(out.model, ToLocalMat4(camera.viewProjection));
    out.tint[0] = instance.tint[0];
    out.tint[1] = instance.tint[1];
    out.tint[2] = instance.tint[2];
    out.tint[3] = instance.tint[3];

    const StaticMeshRenderer::MaterialDefaults fallbackDefaults{};
    const StaticMeshRenderer::MaterialDefaults& defaults = materialDefaults.empty()
        ? fallbackDefaults
        : (materialSlot < materialDefaults.size() ? materialDefaults[materialSlot] : materialDefaults.front());
    std::memcpy(out.materialBaseColor, defaults.baseColor, sizeof(out.materialBaseColor));
    out.materialParams[0] = defaults.metallic;
    out.materialParams[1] = defaults.roughness;
    out.materialParams[2] = defaults.normalStrength;
    out.materialParams[3] = defaults.aoStrength;
    out.materialEmissive[0] = defaults.emissive[0];
    out.materialEmissive[1] = defaults.emissive[1];
    out.materialEmissive[2] = defaults.emissive[2];
    out.materialEmissive[3] = 1.0f;
    out.materialUv[0] = 1.0f;
    out.materialUv[1] = 1.0f;
    out.materialUv[2] = 0.0f;
    out.materialUv[3] = 0.0f;

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

std::vector<char> ReadBinaryFile(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        LogFormat("[STATIC-MESH] failed to open shader: %s", path.c_str());
        std::abort();
    }
    return std::vector<char>(bytes->begin(), bytes->end());
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
    std::string* error)
{
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);
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
    constexpr fastgltf::Options options =
        fastgltf::Options::DecomposeNodeMatrices |
        fastgltf::Options::LoadExternalBuffers |
        fastgltf::Options::LoadExternalImages |
        fastgltf::Options::GenerateMeshIndices;

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
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
    return defaults;
}

RgbaImage CreateFallbackWhiteImage(const std::string& modelPath)
{
    RgbaImage image{};
    image.name = modelPath + "#fallback-white";
    image.width = 4;
    image.height = 4;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.pixels.assign(static_cast<size_t>(image.width) * image.height * 4u, 0xff);
    return image;
}

RgbaImage CreateFallbackNormalImage(const std::string& modelPath)
{
    RgbaImage image{};
    image.name = modelPath + "#fallback-normal";
    image.width = 4;
    image.height = 4;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    VkFormat format,
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
                VK_FORMAT_R8G8B8A8_SRGB, "baseColorTexture", diffuse);
        if (material.normalTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.normalTexture->textureIndex,
                VK_FORMAT_R8G8B8A8_UNORM, "normalTexture", normal);
        if (material.pbrData.metallicRoughnessTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.pbrData.metallicRoughnessTexture->textureIndex,
                VK_FORMAT_R8G8B8A8_UNORM, "metallicRoughnessTexture", orm);
        else if (material.occlusionTexture.has_value())
            loadedAny |= DecodeGltfTexture(asset, material.occlusionTexture->textureIndex,
                VK_FORMAT_R8G8B8A8_UNORM, "occlusionTexture", orm);
        if (loadedAny)
            break;
    }
    return loadedAny;
}

VkShaderModule CreateShaderModule(VkDevice device, client::asset::IAssetReader& assets, const std::string& path)
{
    const std::vector<char> code = ReadBinaryFile(assets, path);
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &create, nullptr, &module));
    return module;
}

VkCommandBuffer BeginOneTimeCommands(VkDevice vkDevice, uint32_t queueFamily, VkCommandPool& pool)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(vkDevice, &alloc, &cmd));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

void EndOneTimeCommands(VkDevice vkDevice, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));
    vkDestroyCommandPool(vkDevice, pool, nullptr);
}

VkFence SubmitOneTimeCommandsNoWait(VkDevice vkDevice, VkQueue queue, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fenceCreate{};
    fenceCreate.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(vkDevice, &fenceCreate, nullptr, &fence));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    return fence;
}

bool CreateHostVisibleBuffer(VulkanDevice& device, VkDevice vkDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, StaticMeshRenderer::Buffer& out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = usage;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vkDevice, &buffer, nullptr, &out.buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vkDevice, out.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(vkDevice, out.buffer, out.memory, 0));

    if (initialData)
    {
        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(vkDevice, out.memory, 0, size, 0, &mapped));
        std::memcpy(mapped, initialData, static_cast<size_t>(size));
        vkUnmapMemory(vkDevice, out.memory);
    }
    return true;
}

void CopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
}

bool CreateDeviceLocalBuffer(VulkanDevice& device, VkDevice vkDevice, VkQueue queue, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, StaticMeshRenderer::Buffer& out)
{
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vkDevice, &buffer, nullptr, &out.buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vkDevice, out.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(vkDevice, out.buffer, out.memory, 0));

    if (initialData && size > 0)
    {
        StaticMeshRenderer::Buffer staging{};
        CreateHostVisibleBuffer(device, vkDevice, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, initialData, staging);
        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = BeginOneTimeCommands(vkDevice, device.GetGraphicsQueueFamily(), uploadPool);
        CopyBuffer(cmd, staging.buffer, out.buffer, size);
        EndOneTimeCommands(vkDevice, queue, uploadPool, cmd);
        if (staging.buffer)
            vkDestroyBuffer(vkDevice, staging.buffer, nullptr);
        if (staging.memory)
            vkFreeMemory(vkDevice, staging.memory, nullptr);
    }
    return true;
}

bool CreateDeviceLocalImage(VulkanDevice& device, VkDevice vkDevice, uint32_t width, uint32_t height,
    VkFormat format, VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {width, height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vkDevice, &create, nullptr, &image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vkDevice, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(vkDevice, &alloc, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(vkDevice, image, memory, 0));
    return true;
}

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
}

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

bool StaticMeshRenderer::Create(VulkanDevice& device, client::asset::IAssetReader& assets, const std::string& modelPath)
{
    Destroy();
    m_device = device.GetDevice();
    m_assets = &assets;
    m_modelPath = modelPath;
    m_status = LoadStatus::Failed;
    bool loaded = false;
    bool buffers = false;
    bool textures = false;
    bool descriptors = false;
    bool pipeline = false;
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

    bool isSkinned = false;
    std::string error;
    if (!DetectSkinnedGltf(assets, modelPath, isSkinned, &error))
    {
        LogFormat("[STATIC-MESH] inspect failed: %s reason=%s", modelPath.c_str(), error.c_str());
        logCreateState();
        return false;
    }
    if (isSkinned)
    {
        m_status = LoadStatus::UnsupportedSkinned;
        LogFormat("[STATIC-MESH] skinned glTF detected, static renderer will not load it: %s", modelPath.c_str());
        logCreateState();
        return false;
    }

    if (!LoadStaticGltfMesh(modelPath))
    {
        logCreateState();
        return false;
    }
    loaded = true;
    if (!CreateBuffers(device))
    {
        logCreateState();
        return false;
    }
    buffers = HasVertexBuffer() && HasIndexBuffer();
    if (!CreateTextures(device, modelPath))
    {
        logCreateState();
        return false;
    }
    textures = HasTexture();
    if (!CreateDescriptors())
    {
        logCreateState();
        return false;
    }
    descriptors = HasDescriptors();
    if (!CreatePipeline(device))
    {
        logCreateState();
        return false;
    }
    pipeline = HasPipeline();

    m_status = LoadStatus::LoadedStatic;
    logCreateState();
    LogFormat("[STATIC-MESH] loaded: %s verts=%zu indices=%zu draws=%zu",
        modelPath.c_str(), m_vertices.size(), m_indices.size(), m_draws.size());
    return true;
}

bool StaticMeshRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_assets || m_status != LoadStatus::LoadedStatic)
        return false;
    DestroyPipeline();
    return CreatePipeline(device);
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
        diag.bufferValid = m_indexBuffer.buffer != VK_NULL_HANDLE;
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
        diag.bufferValid = existing->second.buffer.buffer != VK_NULL_HANDLE;
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

void StaticMeshRenderer::SetMainRenderPass(VkRenderPass renderPass)
{
    m_mainRenderPass = renderPass;
}

bool StaticMeshRenderer::LoadStaticGltfMesh(const std::string& modelPath)
{
    if (!m_assets)
        return false;
    std::string error;
    auto parsed = ParseGltf(*m_assets, modelPath, &error);
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
    LogFormat("[MPERF] mesh=%s verts=%zu submeshes=%zu materials=%u alpha=%s",
        modelPath.c_str(),
        m_vertices.size(),
        m_draws.size(),
        m_materialSlotCount,
        m_alphaModeName.c_str());
    return true;
}

bool StaticMeshRenderer::CreateBuffers(VulkanDevice& device)
{
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);
    CreateDeviceLocalBuffer(device, m_device, graphicsQueue, sizeof(Vertex) * m_vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_vertices.data(), m_vertexBuffer);
    CreateDeviceLocalBuffer(device, m_device, graphicsQueue, sizeof(uint32_t) * m_indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_indices.data(), m_indexBuffer);
    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (Buffer& buffer : frameBuffers)
        {
            CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
        }
    }
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        m_instanceBufferCapacity[frame] = kInitialInstanceCapacity;
        CreateHostVisibleBuffer(device,
            m_device,
            sizeof(StaticMeshInstanceBlock) * m_instanceBufferCapacity[frame],
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            nullptr,
            m_instanceBuffers[frame]);
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

bool StaticMeshRenderer::EnsureLodBuffers(VulkanDevice& device, const LodConfig& config, std::uint64_t configHash, std::uint32_t entityId)
{
    if (configHash == 0 || m_vertices.empty() || m_indices.empty() || m_draws.empty())
        return false;
    ApplyPendingLodResult(device, configHash);
    auto cached = m_lodBuffers.find(configHash);
    if (cached != m_lodBuffers.end())
        return cached->second.buffer.buffer != VK_NULL_HANDLE;
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
        if (ApplyPendingLodResult(device, configHash))
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

bool StaticMeshRenderer::ApplyPendingLodResult(VulkanDevice& device, std::uint64_t configHash)
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

    return QueueLodUpload(device, configHash, std::move(cpuSet));
}

bool StaticMeshRenderer::QueueLodUpload(VulkanDevice& device, std::uint64_t configHash, LodCpuSet&& cpuSet)
{
    if (cpuSet.indices.empty() || HasPendingLodUpload(configHash))
        return false;

    const auto begin = std::chrono::steady_clock::now();
    const VkDeviceSize indexBytes = sizeof(std::uint32_t) * cpuSet.indices.size();
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    LodIndexBuffer lodSet{};
    lodSet.indices = std::move(cpuSet.indices);
    lodSet.draws = std::move(cpuSet.draws);
    lodSet.triangles = cpuSet.triangles;
    lodSet.levels = cpuSet.levels;
    lodSet.quality = cpuSet.quality;
    lodSet.source = cpuSet.source;
    if (!CreateDeviceLocalBuffer(device,
            m_device,
            graphicsQueue,
            indexBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            nullptr,
            lodSet.buffer))
    {
        return false;
    }
    lodSet.generated = true;

    PendingLodUpload upload{};
    upload.configHash = configHash;
    upload.diagnosticEntityId = cpuSet.diagnosticEntityId;
    upload.lodSet = std::move(lodSet);
    if (!CreateHostVisibleBuffer(device,
            m_device,
            indexBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            upload.lodSet.indices.data(),
            upload.staging))
    {
        DestroyBuffer(upload.lodSet.buffer);
        return false;
    }

    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), upload.commandPool);
    CopyBuffer(cmd, upload.staging.buffer, upload.lodSet.buffer.buffer, indexBytes);
    upload.fence = SubmitOneTimeCommandsNoWait(m_device, graphicsQueue, cmd);
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
        const VkResult fenceStatus = vkGetFenceStatus(m_device, it->fence);
        if (fenceStatus == VK_NOT_READY)
        {
            ++it;
            continue;
        }
        VK_CHECK(fenceStatus);

        if (it->staging.buffer)
            vkDestroyBuffer(m_device, it->staging.buffer, nullptr);
        if (it->staging.memory)
            vkFreeMemory(m_device, it->staging.memory, nullptr);
        if (it->commandPool)
            vkDestroyCommandPool(m_device, it->commandPool, nullptr);
        if (it->fence)
            vkDestroyFence(m_device, it->fence, nullptr);

        const std::uint32_t populatedEntityId = it->diagnosticEntityId;
        const std::uint32_t populatedLevelCount = it->lodSet.levels;
        auto existing = m_lodBuffers.find(it->configHash);
        if (existing != m_lodBuffers.end())
        {
            m_retiredLodBuffers.push_back(existing->second.buffer);
            existing->second.buffer = {};
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

bool StaticMeshRenderer::CreateTextures(VulkanDevice& device, const std::string& modelPath)
{
    RgbaImage diffuse{};
    RgbaImage normal{};
    RgbaImage orm{};
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

    auto uploadTexture = [&](const RgbaImage& source, Texture& texture) -> bool {
        RgbaImage image = source;
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), image.format, &props);
        const VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((props.optimalTilingFeatures & required) != required)
        {
            image.format = VK_FORMAT_R8G8B8A8_UNORM;
            vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), image.format, &props);
            if ((props.optimalTilingFeatures & required) != required)
                return false;
        }

        VkQueue graphicsQueue = VK_NULL_HANDLE;
        vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

        texture.name = image.name;
        texture.width = image.width;
        texture.height = image.height;
        texture.mipLevels = 1;
        texture.format = image.format;
        CreateDeviceLocalImage(device, m_device, image.width, image.height, image.format, texture.image, texture.memory);

        Buffer staging{};
        CreateHostVisibleBuffer(device, m_device, image.pixels.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, image.pixels.data(), staging);

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
        TransitionImageLayout(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {image.width, image.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        TransitionImageLayout(cmd, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
        DestroyBuffer(staging);

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = texture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = texture.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &texture.view));

        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.minLod = 0.0f;
        sampler.maxLod = 1.0f;
        if (device.SupportsSamplerAnisotropy())
        {
            sampler.anisotropyEnable = VK_TRUE;
            sampler.maxAnisotropy = device.GetMaxSamplerAnisotropy();
        }
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &texture.sampler));
        return true;
    };

    return uploadTexture(diffuse, m_texture) &&
        uploadTexture(normal, m_normalTexture) &&
        uploadTexture(orm, m_ormTexture);
}

bool StaticMeshRenderer::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding diffuse{};
    diffuse.binding = 1;
    diffuse.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    diffuse.descriptorCount = 1;
    diffuse.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding normal = diffuse;
    normal.binding = 2;
    VkDescriptorSetLayoutBinding orm = diffuse;
    orm.binding = 3;

    VkDescriptorSetLayoutBinding instances{};
    instances.binding = 4;
    instances.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instances.descriptorCount = 1;
    instances.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 5> bindings = {ubo, diffuse, normal, orm, instances};
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight * kUniformSlots;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight * kUniformSlots * 3u;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = kFramesInFlight * kUniformSlots;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight * kUniformSlots;
    pool.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pool.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    std::array<VkDescriptorSetLayout, kFramesInFlight * kUniformSlots> layouts{};
    layouts.fill(m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();

    std::array<VkDescriptorSet, kFramesInFlight * kUniformSlots> flatSets{};
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, flatSets.data()));

    uint32_t setIndex = 0;
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t uniformSlot = 0; uniformSlot < kUniformSlots; ++uniformSlot)
        {
            VkDescriptorSet descriptorSet = flatSets[setIndex++];
            m_descriptorSets[frame][uniformSlot] = descriptorSet;

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = m_uniformBuffers[frame][uniformSlot].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBlock);

            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = m_texture.sampler;
            imageInfo.imageView = m_texture.view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo normalInfo{};
            normalInfo.sampler = m_normalTexture.sampler;
            normalInfo.imageView = m_normalTexture.view;
            normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo ormInfo{};
            ormInfo.sampler = m_ormTexture.sampler;
            ormInfo.imageView = m_ormTexture.view;
            ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo instanceInfo{};
            instanceInfo.buffer = m_instanceBuffers[frame].buffer;
            instanceInfo.offset = 0;
            instanceInfo.range = VK_WHOLE_SIZE;

            std::array<VkWriteDescriptorSet, 5> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptorSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &bufferInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptorSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &imageInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = descriptorSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &normalInfo;
            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3].dstSet = descriptorSet;
            writes[3].dstBinding = 3;
            writes[3].descriptorCount = 1;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].pImageInfo = &ormInfo;
            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[4].dstSet = descriptorSet;
            writes[4].dstBinding = 4;
            writes[4].descriptorCount = 1;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[4].pBufferInfo = &instanceInfo;
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
    return true;
}

void StaticMeshRenderer::UpdateInstanceDescriptorSets(uint32_t frameIndex)
{
    if (frameIndex >= kFramesInFlight || !m_descriptorPool || !m_instanceBuffers[frameIndex].buffer)
        return;

    VkDescriptorBufferInfo instanceInfo{};
    instanceInfo.buffer = m_instanceBuffers[frameIndex].buffer;
    instanceInfo.offset = 0;
    instanceInfo.range = VK_WHOLE_SIZE;

    std::array<VkWriteDescriptorSet, kUniformSlots> writes{};
    for (uint32_t uniformSlot = 0; uniformSlot < kUniformSlots; ++uniformSlot)
    {
        writes[uniformSlot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[uniformSlot].dstSet = m_descriptorSets[frameIndex][uniformSlot];
        writes[uniformSlot].dstBinding = 4;
        writes[uniformSlot].descriptorCount = 1;
        writes[uniformSlot].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[uniformSlot].pBufferInfo = &instanceInfo;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool StaticMeshRenderer::EnsureInstanceCapacity(VulkanDevice& device, uint32_t frameIndex, std::uint32_t requiredRecords)
{
    if (frameIndex >= kFramesInFlight)
        return false;
    requiredRecords = std::max<std::uint32_t>(1u, requiredRecords);
    if (m_instanceBufferCapacity[frameIndex] >= requiredRecords && m_instanceBuffers[frameIndex].buffer)
        return true;

    std::uint32_t nextCapacity = std::max<std::uint32_t>(kInitialInstanceCapacity, m_instanceBufferCapacity[frameIndex]);
    while (nextCapacity < requiredRecords)
        nextCapacity *= 2u;

    DestroyBuffer(m_instanceBuffers[frameIndex]);
    CreateHostVisibleBuffer(device,
        m_device,
        sizeof(StaticMeshInstanceBlock) * nextCapacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        nullptr,
        m_instanceBuffers[frameIndex]);
    m_instanceBufferCapacity[frameIndex] = nextCapacity;
    UpdateInstanceDescriptorSets(frameIndex);
    m_lastInstanceBufferRebuilt = true;
    return true;
}

bool StaticMeshRenderer::CreatePipeline(VulkanDevice& device)
{
    if (!m_assets)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/static_mesh_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/static_mesh_ps.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "PSMain";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, normal);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_descriptorSetLayout;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_pipelineLayout));

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = m_pipelineLayout;
    pipeline.renderPass = m_mainRenderPass ? m_mainRenderPass : device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

void StaticMeshRenderer::RenderInWorld(VulkanDevice& device,
    double timeSeconds,
    const WorldCamera& camera,
    const Instance& instance)
{
    std::vector<Instance> instances;
    instances.push_back(instance);
    RenderBatchInWorld(device, timeSeconds, camera, instances);
}

void StaticMeshRenderer::RenderBatchInWorld(VulkanDevice& device,
    double timeSeconds,
    const WorldCamera& camera,
    const std::vector<Instance>& instances)
{
    LodConfig defaultLod{};
    RenderLodBatchInWorld(device, timeSeconds, camera, instances, defaultLod, 0, 0);
}

void StaticMeshRenderer::RenderLodBatchInWorld(VulkanDevice& device,
    double timeSeconds,
    const WorldCamera& camera,
    const std::vector<Instance>& instances,
    const LodConfig& lodConfig,
    std::uint64_t configHash,
    std::uint32_t lodLevel)
{
    m_lastSubmittedDrawCalls = 0;
    m_lastSubmittedInstances = 0;
    m_lastSubmittedIndexCount = 0;
    m_lastUsedFullResFallback = false;
    m_lastMaterialUniformUpdates = 0;
    m_lastOverrideActiveDraws = 0;
    m_lastInstanceBufferBytes = 0;
    m_lastInstanceBufferRebuilt = false;
    if (!m_pipeline || m_indices.empty() || instances.empty() || !device.IsFrameActive())
        return;
    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const std::uint32_t diagnosticEntityId = instances.empty() ? 0u : instances.front().entityId;
    const bool useLodBuffer = configHash != 0 && lodLevel > 0 && EnsureLodBuffers(device, lodConfig, configHash, diagnosticEntityId);
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
        else if (lodSet->buffer.buffer == VK_NULL_HANDLE)
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

    const uint32_t frameIndex = device.GetFrameIndex();
    if (m_worldRenderFrameIndex != frameIndex)
    {
        m_worldRenderFrameIndex = frameIndex;
        m_worldUniformCursor = 0;
    }

    std::vector<StaticMeshInstanceBlock> instanceBlocks;
    std::vector<InstancedDrawCommand> drawCommands;
    const std::size_t drawCountForLevel = lodSet
        ? std::count_if(lodSet->draws.begin(), lodSet->draws.end(), [&](const LodMeshDraw& draw) {
            return draw.firstIndex < lodSet->indices.size();
        })
        : m_draws.size();
    instanceBlocks.reserve(instances.size() * std::max<std::size_t>(1u, drawCountForLevel));
    drawCommands.reserve(drawCountForLevel);
    auto appendDraw = [&](std::uint32_t firstIndex, std::uint32_t indexCount, std::uint32_t materialSlot, std::uint32_t sourceSubmesh) {
        if (indexCount == 0)
            return;
        {
            InstancedDrawCommand command{};
            command.firstIndex = firstIndex;
            command.indexCount = indexCount;
            command.firstInstance = static_cast<uint32_t>(instanceBlocks.size());
            command.instanceCount = static_cast<uint32_t>(instances.size());
            command.materialSlot = materialSlot;
            command.sourceSubmesh = sourceSubmesh;
            for (const Instance& instance : instances)
            {
                StaticMeshInstanceBlock block{};
                FillStaticMeshInstanceBlock(camera, instance, materialSlot, m_materialDefaults, block);
                instanceBlocks.push_back(block);
                const bool overrideActive = std::any_of(instance.materialOverrides.begin(),
                    instance.materialOverrides.end(),
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

    if (instanceBlocks.empty() || !EnsureInstanceCapacity(device, frameIndex, static_cast<std::uint32_t>(instanceBlocks.size())))
        return;
    m_lastInstanceBufferBytes = instanceBlocks.size() * sizeof(StaticMeshInstanceBlock);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device,
        m_instanceBuffers[frameIndex].memory,
        0,
        static_cast<VkDeviceSize>(m_lastInstanceBufferBytes),
        0,
        &mapped));
    std::memcpy(mapped, instanceBlocks.data(), m_lastInstanceBufferBytes);
    vkUnmapMemory(m_device, m_instanceBuffers[frameIndex].memory);

    VkCommandBuffer cmd = device.GetCommandBuffer();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, lodSet ? lodSet->buffer.buffer : m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    const auto findInstanceIndex = [&](std::uint32_t entityId) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            if (instances[i].entityId == entityId)
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
            if (instances[i].entityId != 3u)
            {
                refInstanceIndex = i;
                break;
            }
        }
    }
    bool loggedMaterialE3 = false;
    bool loggedMaterialRef = false;
    const std::uint64_t frameNumber = device.GetFrameNumber();
    const VkBuffer boundIndexBuffer = lodSet ? lodSet->buffer.buffer : m_indexBuffer.buffer;
    const auto logDrawDiag = [&](const char* tag,
                                 std::uint32_t entityId,
                                 const InstancedDrawCommand& draw,
                                 std::size_t instanceIndex,
                                 uint32_t uniformSlot) {
        const std::size_t absoluteInstance = static_cast<std::size_t>(draw.firstInstance) + instanceIndex;
        const std::size_t instanceOffset = absoluteInstance * sizeof(StaticMeshInstanceBlock);
        LogFormat("%s entity=%u submesh=%u frame=%llu pipeline=0x%llx vbuf=0x%llx vbufOffset=0 vbufRange=%zu ibuf=0x%llx ibufOffset=0 indexCount=%u indexType=UINT32 ibuf_first=%u ibuf_vertexOffset=0 instbuf=0x%llx instbufOffset=%zu instCount=%u firstInstance=%u descSets=[set0=0x%llx] pushConst_bytes=<none> pushConst_size=0",
            tag,
            entityId,
            draw.sourceSubmesh,
            static_cast<unsigned long long>(frameNumber),
            VkHandleValue(m_pipeline),
            VkHandleValue(m_vertexBuffer.buffer),
            sizeof(Vertex) * m_vertices.size(),
            VkHandleValue(boundIndexBuffer),
            draw.indexCount,
            draw.firstIndex,
            VkHandleValue(m_instanceBuffers[frameIndex].buffer),
            instanceOffset,
            draw.instanceCount,
            draw.firstInstance,
            VkHandleValue(m_descriptorSets[frameIndex][uniformSlot]));
    };
    const auto logMaterialDiag = [&](const char* tag,
                                     const Instance& instance,
                                     const StaticMeshInstanceBlock& block,
                                     uint32_t materialSlot,
                                     std::size_t absoluteInstance,
                                     bool isLodActive) {
        const std::size_t instanceOffset = absoluteInstance * sizeof(StaticMeshInstanceBlock);
        const std::size_t materialOffset = instanceOffset + offsetof(StaticMeshInstanceBlock, materialBaseColor);
        const std::size_t materialSize =
            sizeof(block.materialBaseColor) +
            sizeof(block.materialParams) +
            sizeof(block.materialEmissive) +
            sizeof(block.materialUv);
        const std::size_t transformOffset = instanceOffset + offsetof(StaticMeshInstanceBlock, model);
        const float alphaCutoff = m_alphaModeName == "mask" ? 0.5f : 0.0f;
        LogFormat("%s entity=%u frame=%llu alphaMode=%s materialIndex=%u materialBuffer=0x%llx matOffset=%zu matSize=%zu baseColorView=0x%llx normalView=0x%llx metallicRoughnessView=0x%llx baseColorFactor=(%.3f,%.3f,%.3f,%.3f) alphaCutoff=%.3f transformBuffer=0x%llx tfOffset=%zu worldMatrix.row0=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row1=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row2=(%.3f,%.3f,%.3f,%.3f) worldMatrix.row3=(%.3f,%.3f,%.3f,%.3f) isLodActive=%s",
            tag,
            instance.entityId,
            static_cast<unsigned long long>(frameNumber),
            AlphaModeForLog(m_alphaModeName),
            materialSlot,
            VkHandleValue(m_instanceBuffers[frameIndex].buffer),
            materialOffset,
            materialSize,
            VkHandleValue(m_texture.view),
            VkHandleValue(m_normalTexture.view),
            VkHandleValue(m_ormTexture.view),
            block.materialBaseColor[0],
            block.materialBaseColor[1],
            block.materialBaseColor[2],
            block.materialBaseColor[3],
            alphaCutoff,
            VkHandleValue(m_instanceBuffers[frameIndex].buffer),
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
    for (const InstancedDrawCommand& draw : drawCommands)
    {
        const uint32_t uniformSlot = std::min(m_worldUniformCursor++, kUniformSlots - 1);
        UpdateWorldUniform(frameIndex, uniformSlot, camera, instances.front(), timeSeconds, 0u);
        ++m_lastMaterialUniformUpdates;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
            0, 1, &m_descriptorSets[frameIndex][uniformSlot], 0, nullptr);
        if (configHash != 0 && diagnosticInstanceIndex)
        {
            const std::size_t absoluteInstance = static_cast<std::size_t>(draw.firstInstance) + *diagnosticInstanceIndex;
            if (absoluteInstance < instanceBlocks.size())
            {
                if (!loggedMaterialE3)
                {
                    logMaterialDiag("[LOD-MAT-E3]",
                        instances[*diagnosticInstanceIndex],
                        instanceBlocks[absoluteInstance],
                        draw.materialSlot,
                        absoluteInstance,
                        true);
                    loggedMaterialE3 = true;
                }
                logDrawDiag("[LOD-DRAW-E3]", diagnosticEntityId, draw, *diagnosticInstanceIndex, uniformSlot);
            }
        }
        if (refInstanceIndex)
        {
            const std::size_t absoluteInstance = static_cast<std::size_t>(draw.firstInstance) + *refInstanceIndex;
            if (absoluteInstance < instanceBlocks.size())
            {
                if (!loggedMaterialRef)
                {
                    logMaterialDiag("[LOD-MAT-REF]",
                        instances[*refInstanceIndex],
                        instanceBlocks[absoluteInstance],
                        draw.materialSlot,
                        absoluteInstance,
                        false);
                    loggedMaterialRef = true;
                }
                logDrawDiag("[LOD-DRAW-REF]", instances[*refInstanceIndex].entityId, draw, *refInstanceIndex, uniformSlot);
            }
        }
        vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, draw.firstIndex, 0, draw.firstInstance);
        ++m_lastSubmittedDrawCalls;
        m_lastSubmittedIndexCount += draw.indexCount;
    }
    m_lastSubmittedInstances = static_cast<std::uint32_t>(instances.size());
}

void StaticMeshRenderer::DestroyPipeline()
{
    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void StaticMeshRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void StaticMeshRenderer::DestroyTexture(Texture& texture)
{
    if (texture.sampler)
        vkDestroySampler(m_device, texture.sampler, nullptr);
    if (texture.view)
        vkDestroyImageView(m_device, texture.view, nullptr);
    if (texture.image)
        vkDestroyImage(m_device, texture.image, nullptr);
    if (texture.memory)
        vkFreeMemory(m_device, texture.memory, nullptr);
    texture = {};
}

void StaticMeshRenderer::Destroy()
{
    StopLodWorker();
    if (!m_device)
        return;
    DestroyPipeline();
    if (m_descriptorPool)
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    if (m_descriptorSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    m_descriptorSetLayout = VK_NULL_HANDLE;
    DestroyBuffer(m_vertexBuffer);
    DestroyBuffer(m_indexBuffer);
    for (auto& [_, lodSet] : m_lodBuffers)
        DestroyBuffer(lodSet.buffer);
    m_lodBuffers.clear();
    for (PendingLodUpload& upload : m_pendingLodUploads)
    {
        if (upload.fence)
        {
            vkWaitForFences(m_device, 1, &upload.fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(m_device, upload.fence, nullptr);
        }
        if (upload.commandPool)
            vkDestroyCommandPool(m_device, upload.commandPool, nullptr);
        DestroyBuffer(upload.staging);
        DestroyBuffer(upload.lodSet.buffer);
    }
    m_pendingLodUploads.clear();
    for (Buffer& buffer : m_retiredLodBuffers)
        DestroyBuffer(buffer);
    m_retiredLodBuffers.clear();
    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (Buffer& buffer : frameBuffers)
            DestroyBuffer(buffer);
    }
    for (Buffer& buffer : m_instanceBuffers)
        DestroyBuffer(buffer);
    m_instanceBufferCapacity = {};
    DestroyTexture(m_texture);
    DestroyTexture(m_normalTexture);
    DestroyTexture(m_ormTexture);
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
    m_device = VK_NULL_HANDLE;
}

void StaticMeshRenderer::UpdateWorldUniform(uint32_t frameIndex,
    uint32_t uniformSlot,
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
    const MaterialDefaults fallbackDefaults{};
    const MaterialDefaults& defaults = m_materialDefaults.empty()
        ? fallbackDefaults
        : (materialSlot < m_materialDefaults.size() ? m_materialDefaults[materialSlot] : m_materialDefaults.front());
    std::memcpy(uniform.materialBaseColor, defaults.baseColor, sizeof(uniform.materialBaseColor));
    uniform.materialParams[0] = defaults.metallic;
    uniform.materialParams[1] = defaults.roughness;
    uniform.materialParams[2] = defaults.normalStrength;
    uniform.materialParams[3] = defaults.aoStrength;
    uniform.materialEmissive[0] = defaults.emissive[0];
    uniform.materialEmissive[1] = defaults.emissive[1];
    uniform.materialEmissive[2] = defaults.emissive[2];
    uniform.materialEmissive[3] = 1.0f;
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

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}
