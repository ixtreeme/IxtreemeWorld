#include "asset/ExrImage.h"
#include "SkinnedMeshRenderer.h"
#include "ViewImpostorRenderer.h"

#include "JobSystem.h"

#include "AssimpImporter.h"
#include "Debug.h"
#include "IXRHIShader.h"
#include "math/IXMath.h"
#include "asset/IAssetReader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR  // Radiance .hdr sky panoramas (SkyRenderer)
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct SkinnedMeshRenderer::PoseScratch
{
    ozz::animation::SamplingJob::Context context;
    std::vector<ozz::math::SoaTransform> locals;
    std::vector<ozz::math::Float4x4> models;
    std::vector<ixtreeme::math::Mat4> palette;
};

struct SkinnedMeshRenderer::OzzRuntime
{
    ozz::animation::Skeleton skeleton;
    ozz::animation::Animation idle;
    ozz::animation::Animation walk;
    ozz::animation::Animation run;
    ozz::animation::SamplingJob::Context context;
    std::vector<ozz::math::SoaTransform> locals;
    std::vector<ozz::math::Float4x4> models;
    bool hasIdle = false;
    bool hasWalk = false;
    bool hasRun = false;
};

namespace
{
namespace xm = ixtreeme::math;

void Log(const char* text)
{
    Tracen(text);
}

void LogFormat(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

unsigned long long RhiObjectId(const void* object)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object));
}

using Mat4 = ixtreeme::math::Mat4;

Mat4 Translation(float x, float y, float z);

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

// Uniform slots' UniformBlocks share a page's buffer this far apart: a multiple of 256 bytes, the
// most any device asks uniform buffer offsets to be aligned to.
constexpr std::uint64_t kUniformStride = (sizeof(UniformBlock) + 255u) & ~std::uint64_t{255u};

static_assert(sizeof(SkinnedMeshRenderer::Vertex) == 32, "Graphics vertex layout must stay 32 bytes");

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

void FillWaterUniform(const WaterConfig& water, double timeSeconds, UniformBlock& uniform)
{
    uniform.waterParams[0] = water.enabled ? 1.0f : 0.0f;
    uniform.waterParams[1] = water.waterLevelY;
    uniform.waterParams[2] = static_cast<float>(static_cast<int>(water.causticMode));
    uniform.waterParams[3] = std::clamp(water.causticIntensity, 0.0f, 3.0f);
    uniform.causticParams[0] = std::clamp(water.causticScale, 0.05f, 2.0f);
    uniform.causticParams[1] = static_cast<float>(timeSeconds) * std::clamp(water.causticSpeed, 0.0f, 2.0f);
    uniform.causticParams[2] = std::clamp(water.causticMaxDepth, 1.0f, 30.0f);
    uniform.causticParams[3] = 0.0f;
}

struct DdsImage
{
    // Decoded RGBA level-0 payload (all live producers are single-level RGBA;
    // the old multi-mip/compressed DDS path had no callers and was removed).
    std::string filename;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    ixrhi::IXRHIFormat format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    std::vector<uint8_t> pixels;
};

const char* RhiFormatName(ixrhi::IXRHIFormat format)
{
    switch (format)
    {
    case ixrhi::IXRHIFormat::R8G8B8A8Unorm: return "R8G8B8A8_UNORM";
    case ixrhi::IXRHIFormat::R8G8B8A8Srgb: return "R8G8B8A8_SRGB";
    case ixrhi::IXRHIFormat::B8G8R8A8Unorm: return "B8G8R8A8_UNORM";
    case ixrhi::IXRHIFormat::B8G8R8A8Srgb: return "B8G8R8A8_SRGB";
    default: break;
    }
    return "UNDEFINED";
}

Mat4 Identity()
{
    return xm::Mat4Identity();
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    return xm::MultiplyRowMajor(a, b);
}

Mat4 Scale(float value)
{
    return xm::Scale({value, value, value});
}

Mat4 RotationY(float angle)
{
    return xm::RotationYRowMajor(angle);
}

Mat4 Translation(float x, float y, float z)
{
    return xm::Translation({x, y, z});
}

Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    return xm::PerspectiveVulkan(fovYRadians, aspect, zNear, zFar);
}

Mat4 ToLocalMat4(const WorldMat4& matrix)
{
    Mat4 r{};
    std::memcpy(r.m, matrix.m, sizeof(r.m));
    return r;
}

const char* MotionStateName(SkinnedMeshRenderer::MotionState state)
{
    switch (state)
    {
    case SkinnedMeshRenderer::MotionState::Idle: return "wait";
    case SkinnedMeshRenderer::MotionState::Walk: return "walk";
    case SkinnedMeshRenderer::MotionState::Run: return "run";
    default: return "unknown";
    }
}

// NOTE (Phase 3D): the dead multi-mip/compressed DDS loader (format-name
// helpers, LoadDdsImage, ReadBinaryFile) was deleted - it had no callers. All live texture producers yield
// single-level RGBA via CreateRgbaImage/CreateFallbackWhiteDdsImage.

DdsImage CreateFallbackWhiteDdsImage(const std::string& sourcePath)
{
    DdsImage out{};
    const size_t slash = sourcePath.find_last_of("\\/");
    const std::string filename = slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);

    out.filename = filename + " (fallback white)";
    out.width = 4;
    out.height = 4;
    out.mipLevels = 1;
    out.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height * 4u, 0xff);
    return out;
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
    const size_t length = byteLength == std::numeric_limits<size_t>::max()
        ? available
        : byteLength;
    if (length > available)
        return false;

    const auto* begin = reinterpret_cast<const uint8_t*>(data + byteOffset);
    out.assign(begin, begin + length);
    return true;
}

DdsImage CreateRgbaImage(std::string name, uint32_t width, uint32_t height, std::vector<uint8_t> pixels)
{
    DdsImage out{};
    out.filename = std::move(name);
    out.width = width;
    out.height = height;
    out.mipLevels = 1;
    out.format = ixrhi::IXRHIFormat::R8G8B8A8Srgb;
    out.pixels = std::move(pixels);
    return out;
}

bool LoadGltfBaseColorTexture(client::asset::IAssetReader& assets,
    const std::string& modelPath, DdsImage& out)
{
    const size_t slash = modelPath.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);

    auto modelBytes = assets.ReadAll(modelPath);
    if (!modelBytes)
        return false;

    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(modelBytes->data()), modelBytes->size());
    if (data.error() != fastgltf::Error::None)
        return false;

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
        fastgltf::Options::DecomposeNodeMatrices);
    if (assetResult.error() != fastgltf::Error::None)
        return false;

    fastgltf::Asset asset = std::move(assetResult.get());
    for (const auto& material : asset.materials)
    {
        if (!material.pbrData.baseColorTexture.has_value())
            continue;

        const size_t textureIndex = material.pbrData.baseColorTexture->textureIndex;
        if (textureIndex >= asset.textures.size())
            continue;

        const auto& texture = asset.textures[textureIndex];
        if (!texture.imageIndex.has_value() || texture.imageIndex.value() >= asset.images.size())
            continue;

        const auto& image = asset.images[texture.imageIndex.value()];
        std::vector<uint8_t> encoded;
        if (!CopyDataSourceBytes(asset, image.data, 0, std::numeric_limits<size_t>::max(), encoded) ||
            encoded.empty())
            continue;

        if (client::asset::IsExr(encoded))
        {
            std::string error;
            auto exr = client::asset::DecodeExr(encoded, error);
            if (!exr) { LogFormat("[EXR] %s", error.c_str()); continue; }
            out = CreateRgbaImage(std::string(image.name.empty() ? "glTF baseColorTexture" : image.name),
                static_cast<std::uint32_t>(exr->width), static_cast<std::uint32_t>(exr->height), client::asset::ExrHalfPixels(*exr));
            out.format = ixrhi::IXRHIFormat::R16G16B16A16Float;
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
            continue;
        }

        std::vector<uint8_t> pixels(
            decoded, decoded + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
        stbi_image_free(decoded);

        out = CreateRgbaImage(std::string(image.name.empty() ? "glTF baseColorTexture" : image.name),
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(pixels));
        LogFormat("[GLTF-TEX] decoded baseColorTexture material='%s' image='%s' (%ux%u, source channels=%d)",
            material.name.c_str(),
            out.filename.c_str(),
            out.width,
            out.height,
            channels);
        return true;
    }

    return false;
}

bool LoadRgbaTextureFile(const std::filesystem::path& path, DdsImage& out)
{
    if (client::asset::IsExrPath(path))
    {
        std::string error;
        auto exr = client::asset::LoadExr(path, error);
        if (!exr) { LogFormat("[EXR] %s", error.c_str()); return false; }
        out = CreateRgbaImage(path.filename().generic_string(), static_cast<std::uint32_t>(exr->width),
            static_cast<std::uint32_t>(exr->height), client::asset::ExrHalfPixels(*exr));
        out.format = ixrhi::IXRHIFormat::R16G16B16A16Float;
        return true;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return false;
    }

    std::vector<uint8_t> pixels(
        decoded, decoded + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
    stbi_image_free(decoded);

    out = CreateRgbaImage(path.filename().generic_string(),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        std::move(pixels));
    LogFormat("[FBX-IMPORT] skinned diffuse texture loaded path=%s (%ux%u, source channels=%d)",
        path.generic_string().c_str(),
        out.width,
        out.height,
        channels);
    return true;
}

std::vector<std::uint32_t> ReadSpirv(client::asset::IAssetReader& assets, const std::string& path)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0)
    {
        LogFormat("[MESH] failed to open shader: %s", path.c_str());
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

struct PreviewRectResult
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

PreviewRectResult PreviewRect(uint32_t extentWidth, uint32_t extentHeight)
{
    // TODO: derive this from the RmlUi lobby preview-frame bounds instead of mirroring the current fixed layout.
    const int32_t cardWidth = 940;
    const int32_t cardHeight = 492;
    const int32_t cardX = std::max<int32_t>(0, (static_cast<int32_t>(extentWidth) - cardWidth) / 2);
    const int32_t cardY = std::max<int32_t>(0, (static_cast<int32_t>(extentHeight) - cardHeight) / 2);

    PreviewRectResult rect{};
    rect.x = cardX + 46;
    rect.y = cardY + 96;
    rect.width = 224;
    rect.height = 324;

    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (rect.x + static_cast<int32_t>(rect.width) > static_cast<int32_t>(extentWidth))
        rect.width = extentWidth - rect.x;
    if (rect.y + static_cast<int32_t>(rect.height) > static_cast<int32_t>(extentHeight))
        rect.height = extentHeight - rect.y;
    return rect;
}

void Normalize3(float v[3])
{
    xm::interop::NormalizeFloat3(v);
}

void SkinVertex(const SkinnedMeshRenderer::SourceVertex& source,
    const std::vector<Mat4>& bonePalette, float outPosition[3],
    float outNormal[3], float outUv[2], int modelBones[4])
{
    outPosition[0] = outPosition[1] = outPosition[2] = 0.0f;
    outNormal[0] = outNormal[1] = outNormal[2] = 0.0f;
    outUv[0] = outUv[1] = 0.0f;
    modelBones[0] = modelBones[1] = modelBones[2] = modelBones[3] = -1;

    int totalWeight = 0;
    for (int i = 0; i < 4; ++i)
        totalWeight += source.boneWeights[i];

    if (totalWeight <= 0 || bonePalette.empty())
    {
        std::memcpy(outPosition, source.position, sizeof(float) * 3);
        std::memcpy(outNormal, source.normal, sizeof(float) * 3);
        std::memcpy(outUv, source.uv, sizeof(float) * 2);
        Normalize3(outNormal);
        return;
    }

    for (int influence = 0; influence < 4; ++influence)
    {
        const int weightByte = source.boneWeights[influence];
        if (weightByte == 0)
            continue;

        const int modelBone = source.boneIndices[influence];
        modelBones[influence] = modelBone;
        if (modelBone < 0 || modelBone >= static_cast<int>(bonePalette.size()))
            continue;

        const float weight = static_cast<float>(weightByte) / static_cast<float>(totalWeight);
        const float* matrix = bonePalette[modelBone].m;

        float skinnedPosition[3]{};
        float skinnedNormal[3]{};
        xm::interop::TransformPointRowVector(matrix, source.position, skinnedPosition);
        xm::interop::TransformVectorRowVector(matrix, source.normal, skinnedNormal);

        for (int axis = 0; axis < 3; ++axis)
        {
            outPosition[axis] += skinnedPosition[axis] * weight;
            outNormal[axis] += skinnedNormal[axis] * weight;
        }
    }

    Normalize3(outNormal);
    outUv[0] = source.uv[0];
    outUv[1] = source.uv[1];
}

template <typename T>
bool ReadOzzObject(client::asset::IAssetReader& assets, const std::string& path, T& out)
{
    auto bytes = assets.ReadAll(path);
    if (!bytes)
    {
        LogFormat("[OZZ] missing archive: %s", path.c_str());
        return false;
    }

    ozz::io::MemoryStream stream;
    if (stream.Write(bytes->data(), bytes->size()) != bytes->size())
    {
        LogFormat("[OZZ] failed to stage archive in memory: %s", path.c_str());
        return false;
    }
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<T>())
    {
        LogFormat("[OZZ] archive type mismatch: %s", path.c_str());
        return false;
    }
    archive >> out;
    return true;
}

xm::Mat4 IdentityPaletteMatrix()
{
    return xm::Mat4Identity();
}

xm::Mat4 ToRowMajorMatrix(const ozz::math::Float4x4& matrix)
{
    return xm::interop::FromOzzFloat4x4RowMajor(matrix);
}

xm::Mat4 ToRowVectorPaletteMatrix(const ozz::math::Float4x4& matrix)
{
    return xm::interop::FromOzzFloat4x4RowVectorPalette(matrix);
}

ozz::math::Float4x4 ToOzzMatrix(const fastgltf::math::fmat4x4& matrix)
{
    ozz::math::Float4x4 out{};
    for (int col = 0; col < 4; ++col)
    {
        out.cols[col] = ozz::math::simd_float4::Load(
            matrix[col][0], matrix[col][1], matrix[col][2], matrix[col][3]);
    }
    return out;
}

uint8_t ToWeightByte(float weight)
{
    const float clamped = std::clamp(weight, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

uint32_t PackBytes(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
{
    return (b0 & 0xffu) |
        ((b1 & 0xffu) << 8u) |
        ((b2 & 0xffu) << 16u) |
        ((b3 & 0xffu) << 24u);
}
}

SkinnedMeshRenderer::SkinnedMeshRenderer() = default;

SkinnedMeshRenderer::~SkinnedMeshRenderer()
{
    Destroy();
}

bool SkinnedMeshRenderer::Create(ixrhi::IXRHIDevice& rhi,
    client::asset::IAssetReader& assets,
    const std::string& modelPath)
{
    Destroy();
    return LoadCpu(assets, modelPath) && FinishGpu(rhi);
}

bool SkinnedMeshRenderer::LoadCpu(client::asset::IAssetReader& assets, const std::string& modelPath)
{
    m_assets = &assets;
    m_loadedModelPath = modelPath;
    std::string ext = std::filesystem::path(modelPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool loaded = ext == ".fbx" ? LoadFbxMesh(modelPath) : LoadGltfMesh(modelPath);
    if (!loaded)
    {
        LogFormat("[MESH] Create: loaded=0 model=%s", modelPath.c_str());
        return false;
    }
    m_impostorBoneBounds.assign(static_cast<std::size_t>(m_boneCount) + 1, MeshBounds{});
    for (auto& bounds : m_impostorBoneBounds)
        for (int axis = 0; axis < 3; ++axis)
        {
            bounds.min[axis] = INFINITY;
            bounds.max[axis] = -INFINITY;
        }
    for (const auto& vertex : m_restVerticesGpu)
    {
        bool weighted = false;
        for (unsigned influence = 0; influence < 4; ++influence)
        {
            if (((vertex.packedWeights >> (influence * 8)) & 255) == 0)
                continue;
            weighted = true;
            const auto bone = (vertex.packedBones >> (influence * 8)) & 255;
            if (bone >= m_boneCount)
                continue;
            auto& bounds = m_impostorBoneBounds[bone];
            for (int axis = 0; axis < 3; ++axis)
            {
                bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
            }
        }
        if (!weighted)
        {
            auto& bounds = m_impostorBoneBounds.back();
            for (int axis = 0; axis < 3; ++axis)
            {
                bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
            }
        }
    }
    DecodeTextures(modelPath);
    return true;
}

namespace
{
// IX_SKIN_VERIFY=1: each model's GPU skinning is checked against the CPU's when it is made (the GPU is
// waited for: a hitch per model). Off by default.
bool SkinVerifyRequested()
{
    std::string value;
#if defined(_WIN32)
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, "IX_SKIN_VERIFY") == 0 && text)
    {
        value = text;
        std::free(text);
    }
#else
    if (const char* text = std::getenv("IX_SKIN_VERIFY"))
        value = text;
#endif
    return value == "1" || value == "true" || value == "on";
}
} // namespace

bool SkinnedMeshRenderer::FinishGpu(ixrhi::IXRHIDevice& rhi)
{
    m_rhi = &rhi;
    static const bool verify = SkinVerifyRequested();
    m_deferUploads = !verify;
    const bool loaded = m_decodedTextures != nullptr;
    const bool buffers = loaded ? CreateBuffers(rhi) : false;
    const bool compute = buffers ? CreateComputeResources(rhi) : false;
    const bool textures = compute ? UploadDecodedTextures(rhi) : false;
    const bool descriptors = textures ? CreateBindGroup(rhi) : false;
    const bool pipeline = descriptors ? CreatePipeline(rhi) : false;

    LogFormat("[MESH] Create: loaded=%d buffers=%d compute=%d textures=%d descriptors=%d pipeline=%d",
        loaded ? 1 : 0,
        buffers ? 1 : 0,
        compute ? 1 : 0,
        textures ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0);
    if (loaded && buffers && compute && textures && descriptors && pipeline)
    {
        LogFormat("[MESH] Create OK, pipeline=%s", m_pipeline->DebugName().c_str());
        return true;
    }

    Destroy();
    return false;
}

bool SkinnedMeshRenderer::RecreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_rhi)
        return true;

    m_rhi = &rhi;
    DestroyPipeline();
    // Deferred-true while the target pass is torn down (parity); real failures
    // abort in the backend like the pre-migration checked native path.
    CreatePipeline(rhi);
    return true;
}

void SkinnedMeshRenderer::Skin(ixrhi::IXRHICommandList& cmd,
                               const ixrhi::IXRHIFrameInfo& frame,
                               double timeSeconds)
{
    SkinInstance(cmd, frame, 0, m_motionState, static_cast<float>(timeSeconds));
}

void SkinnedMeshRenderer::SkinInstance(ixrhi::IXRHICommandList& cmd,
                                       const ixrhi::IXRHIFrameInfo& frame,
                                       uint32_t skinSlot,
                                       MotionState state,
                                       float animTimeSeconds)
{
    if (!m_computePipeline || !frame.frameActive)
        return;
    if (!EnsureSkinSlot(skinSlot))
        return;
    RecordPendingUploads(cmd, frame);

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;

    const auto recordStart = std::chrono::high_resolution_clock::now();
    if (!UploadBonePalette(state, animTimeSeconds, frameIndex, skinSlot))
    {
        Log("[COMPUTE] Skin skipped: failed to upload bone palette");
        return;
    }

    DispatchSkin(cmd, frameIndex, skinSlot);
    EmitSkinBarrier(cmd, frameIndex, skinSlot);

    const auto recordEnd = std::chrono::high_resolution_clock::now();
    const double recordMs = std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();
    (void)recordMs;
   //if (timeSeconds - m_lastAnimationLogTime >= 1.0)
   //{
   //    LogFormat("[ANIM] gpu-skin animTime=%.2f/%.2f dispatchMs=%.3f",
   //        animTimeSeconds,
   //        0.0f,
   //        recordMs);
   //    m_lastAnimationLogTime = timeSeconds;
   //}
}

void SkinnedMeshRenderer::SkinInstanceFromPose(ixrhi::IXRHICommandList& cmd,
                                               const ixrhi::IXRHIFrameInfo& frame,
                                               uint32_t skinSlot,
                                               ozz::span<const ozz::math::SoaTransform> localPose)
{
    if (!m_computePipeline || !frame.frameActive)
        return;
    if (!EnsureSkinSlot(skinSlot))
        return;
    RecordPendingUploads(cmd, frame);

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (frameIndex >= kFramesInFlight || !BonePalette(frameIndex, skinSlot) ||
        !m_ozz || m_bonePaletteCpu.empty())
    {
        return;
    }
    // The supplied pose must cover this skeleton's joints, or LocalToModelJob would read past it
    // (or skin against a foreign skeleton's data). Fail safe rather than render garbage.
    if (localPose.size() < NumSoaJoints())
        return;

    // Build the GPU bone palette directly from the externally supplied local pose (no clip
    // sampling, no CPU vertex skin), upload it, and dispatch compute skinning for this slot.
    if (!BuildPaletteFromLocals(localPose))
        return;
    if (!UploadPaletteToBuffer(frameIndex, skinSlot))
        return;

    DispatchSkin(cmd, frameIndex, skinSlot);
    EmitSkinBarrier(cmd, frameIndex, skinSlot);
}

const ozz::animation::Skeleton* SkinnedMeshRenderer::Skeleton() const
{
    return m_ozz ? &m_ozz->skeleton : nullptr;
}

ozz::span<const ozz::math::SoaTransform> SkinnedMeshRenderer::RestPoseLocals() const
{
    if (!m_ozz)
        return {};
    return m_ozz->skeleton.joint_rest_poses();
}

std::uint32_t SkinnedMeshRenderer::NumJoints() const
{
    return m_ozz ? static_cast<std::uint32_t>(m_ozz->skeleton.num_joints()) : 0u;
}

std::uint32_t SkinnedMeshRenderer::NumSoaJoints() const
{
    return m_ozz ? static_cast<std::uint32_t>(m_ozz->skeleton.num_soa_joints()) : 0u;
}

std::vector<std::string> SkinnedMeshRenderer::JointNames() const
{
    std::vector<std::string> names;
    if (!m_ozz)
        return names;
    const ozz::span<const char* const> jointNames = m_ozz->skeleton.joint_names();
    names.reserve(jointNames.size());
    for (const char* name : jointNames)
        names.emplace_back(name ? name : "");
    return names;
}

void SkinnedMeshRenderer::Render(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds)
{
    static bool loggedNoPipeline = false;
    static bool loggedFrameInactive = false;
    static bool loggedExtentZero = false;
    static bool loggedRectZero = false;
    static bool loggedDraw = false;

    if (!m_pipeline || m_uniformPages.empty() || !m_indexBuffer || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] Render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }
    if (!SunShadowBound())
        return;

    if (!frame.frameActive || frame.commandList == nullptr)
    {
        if (!loggedFrameInactive)
        {
            Log("[MESH] Render skip: frame inactive");
            loggedFrameInactive = true;
        }
        return;
    }

    const std::uint32_t extentWidth = frame.targetWidth;
    const std::uint32_t extentHeight = frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
    {
        if (!loggedExtentZero)
        {
            Log("[MESH] Render skip: extent 0");
            loggedExtentZero = true;
        }
        return;
    }

    const PreviewRectResult rect = PreviewRect(extentWidth, extentHeight);
    if (rect.width == 0 || rect.height == 0)
    {
        if (!loggedRectZero)
        {
            LogFormat("[MESH] Render skip: preview rect 0 (extent=%ux%u)", extentWidth, extentHeight);
            loggedRectZero = true;
        }
        return;
    }

    if (!loggedDraw)
    {
        LogFormat("[MESH] Render: drawing skinnedMesh in rect x=%d y=%d w=%u h=%u (swapchain %ux%u)",
            rect.x,
            rect.y,
            rect.width,
            rect.height,
            extentWidth,
            extentHeight);
        loggedDraw = true;
    }

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, 0);
    if (!skinned)
        return;
    BeginFrameSlots(frame);
    const std::optional<UniformSlot> uniformSlot = NextUniformSlot(frameIndex);
    if (!uniformSlot)
        return;
    const float aspect = static_cast<float>(rect.width) / static_cast<float>(rect.height);
    UpdateUniform(*uniformSlot, timeSeconds, aspect);

    const std::uint32_t clearX = static_cast<std::uint32_t>(std::max<std::int32_t>(rect.x, 0));
    const std::uint32_t clearY = static_cast<std::uint32_t>(std::max<std::int32_t>(rect.y, 0));
    cmd.ClearDepth(1.0f, clearX, clearY, rect.width, rect.height);

    cmd.SetViewport(static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height));
    cmd.SetScissor(clearX, clearY, rect.width, rect.height);
    cmd.SetGraphicsPipeline(*m_pipeline);

    cmd.SetVertexBuffer(0, *skinned, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    static bool loggedDraws = false;
    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        const MeshDraw& draw = m_draws[i];
        cmd.BindGroup(0, *uniformSlot->page->group, uniformSlot->index * kTextureCount + draw.textureIndex);
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);

        if (!loggedDraws)
        {
            LogFormat("[MESH] drawing mesh[%zu] with texture %s",
                i,
                draw.textureIndex < m_textures.size() ? m_textures[draw.textureIndex].name.c_str() : "<invalid>");
        }
    }
    loggedDraws = true;

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
}

void SkinnedMeshRenderer::RenderInWorld(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    double timeSeconds,
    const WorldCamera& camera,
    WorldVec3 position,
    float yawRadians,
    uint32_t skinSlot,
    std::array<float, 4> tint,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    std::array<float, 3> scale)
{
    static bool loggedDraw = false;
    static bool loggedNoPipeline = false;

    if (!m_pipeline || m_uniformPages.empty() || !m_indexBuffer || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] World render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }
    if (!SunShadowBound())
        return;

    if (!frame.frameActive || frame.commandList == nullptr)
        return;

    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, skinSlot);
    if (!skinned)
        return;
    if (auto* cache = ViewImpostorRenderer::Active(); cache && !cache->IsCapturing())
    {
        // A stable padded box avoids making a new crop for each tiny animation movement. Expand it
        // using the actual pose's bone influence boxes, including externally injected/blended poses.
        const float padding = LocalExtent() * std::max({std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2])});
        WorldVec3 minimum{position.x - padding, position.y - padding, position.z - padding};
        WorldVec3 maximum{position.x + padding, position.y + padding, position.z + padding};
        if (ViewImpostorRenderer::ProjectBounds(camera, minimum, maximum, extentWidth, extentHeight, 80, 256))
        {
            auto* buffer = BonePalette(frameIndex, skinSlot);
            const auto* palette = buffer ? static_cast<const Mat4*>(buffer->HostAddress()) : nullptr;
            if (palette && m_impostorBoneBounds.size() == static_cast<std::size_t>(m_boneCount) + 1)
            {
                const Mat4 model = Multiply(Multiply(xm::Scale({scale[0], scale[1], scale[2]}), RotationY(-yawRadians)),
                                            Translation(position.x, position.y, position.z));
                for (std::size_t bone = 0; bone < m_impostorBoneBounds.size(); ++bone)
                {
                    const auto& bounds = m_impostorBoneBounds[bone];
                    if (bounds.min[0] > bounds.max[0])
                        continue;
                    const Mat4 transform = bone < m_boneCount ? Multiply(palette[bone], model) : model;
                    const WorldVec3 center{(bounds.min[0] + bounds.max[0]) * .5f, (bounds.min[1] + bounds.max[1]) * .5f,
                                           (bounds.min[2] + bounds.max[2]) * .5f};
                    const float extent[] = {(bounds.max[0] - bounds.min[0]) * .5f,
                                            (bounds.max[1] - bounds.min[1]) * .5f,
                                            (bounds.max[2] - bounds.min[2]) * .5f};
                    const WorldVec3 transformed = xm::TransformPoint(transform, center);
                    float reach[3]{};
                    for (int column = 0; column < 3; ++column)
                        for (int row = 0; row < 3; ++row)
                            reach[column] += std::abs(transform.m[row * 4 + column]) * extent[row];
                    minimum.x = std::min(minimum.x, transformed.x - reach[0]);
                    minimum.y = std::min(minimum.y, transformed.y - reach[1]);
                    minimum.z = std::min(minimum.z, transformed.z - reach[2]);
                    maximum.x = std::max(maximum.x, transformed.x + reach[0]);
                    maximum.y = std::max(maximum.y, transformed.y + reach[1]);
                    maximum.z = std::max(maximum.z, transformed.z + reach[2]);
                }
                std::uint64_t revision = 1469598103934665603ull;
                const auto hash = [&](const void* data, std::size_t size) {
                    const auto* bytes = static_cast<const unsigned char*>(data);
                    for (std::size_t i = 0; i < size; ++i)
                        revision = (revision ^ bytes[i]) * 1099511628211ull;
                };
                hash(&position, sizeof(position));
                hash(&yawRadians, sizeof(yawRadians));
                hash(scale.data(), sizeof(scale));
                hash(tint.data(), sizeof(tint));
                const auto capture = [&] {
                    RenderInWorld(cmd, frame, timeSeconds, camera, position, yawRadians, skinSlot, tint,
                                  extentWidth, extentHeight, scale);
                    return static_cast<std::uint64_t>(m_indexCount / 3);
                };
                if (cache->TryDraw(this, static_cast<std::uint64_t>(skinSlot) + 1, revision,
                                   ViewImpostorRenderer::Kind::Character, minimum, maximum, m_indexCount / 3,
                                   std::cref(capture)))
                    return;
            }
        }
    }
    BeginFrameSlots(frame);
    const std::optional<UniformSlot> uniformSlot = NextUniformSlot(frameIndex);
    if (!uniformSlot)
        return;
    UpdateWorldUniform(*uniformSlot, camera, position, yawRadians, timeSeconds, tint, scale);

    if (!ViewImpostorRenderer::ApplyCaptureViewport(cmd))
    {
        cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
        cmd.SetScissor(0, 0, extentWidth, extentHeight);
    }
    cmd.SetGraphicsPipeline(*m_pipeline);

    cmd.SetVertexBuffer(0, *skinned, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    for (const MeshDraw& draw : m_draws)
    {
        cmd.BindGroup(0, *uniformSlot->page->group, uniformSlot->index * kTextureCount + draw.textureIndex);
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }

    if (!loggedDraw)
    {
        LogFormat("[MESH] World render: skinnedMesh pos=(%.2f,%.2f,%.2f) yaw=%.2f camera eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
            position.x,
            position.y,
            position.z,
            yawRadians,
            camera.eye.x,
            camera.eye.y,
            camera.eye.z,
            camera.target.x,
            camera.target.y,
            camera.target.z);
        loggedDraw = true;
    }
}

void SkinnedMeshRenderer::RenderInWorldReflection(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    const WorldCamera& camera,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    const ixrhi::IXRHIRenderPass* renderPass,
    float waterLevelY,
    WorldVec3 position,
    float yawRadians,
    uint32_t skinSlot,
    std::array<float, 4> tint,
    std::array<float, 3> scale)
{
    // Reflection draws run inside the terrain-owned native reflection pass.
    // That pass uses the swapchain color/depth formats (same as the backend
    // main pass and the offscreen scene pass), so a pipeline baked against
    // the borrowed IXRHI pass token is attachment-compatible. The native
    // render-pass handle never crosses into this renderer.
    if (!m_bindLayout || !m_pipeline || m_uniformPages.empty() || !m_indexBuffer ||
        m_indexCount == 0 || !frame.frameActive || frame.commandList == nullptr || m_rhi == nullptr)
        return;
    if (!SunShadowBound())
        return;

    // Bake against the explicit pass when the caller supplies one, else the
    // renderer's own target (offscreen scene pass, or backend default when
    // null). Comparing the EFFECTIVE token also catches m_targetPass swaps
    // while the caller passes null.
    const ixrhi::IXRHIRenderPass* effectivePass = renderPass != nullptr ? renderPass : m_targetPass;
    if (!m_reflectionPipeline || m_reflectionPass != effectivePass)
    {
        DestroyReflectionPipeline();
        if (!CreateReflectionPipeline(*m_rhi, effectivePass))
            return;
    }

    if (targetWidth == 0 || targetHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, skinSlot);
    if (!skinned)
        return;
    BeginFrameSlots(frame);
    const std::optional<UniformSlot> uniformSlot = NextUniformSlot(frameIndex);
    if (!uniformSlot)
        return;
    UpdateWorldUniform(*uniformSlot, camera, position, yawRadians, 0.0, tint, scale, true, waterLevelY);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight));
    cmd.SetScissor(0, 0, targetWidth, targetHeight);
    cmd.SetGraphicsPipeline(*m_reflectionPipeline);

    cmd.SetVertexBuffer(0, *skinned, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    for (const MeshDraw& draw : m_draws)
    {
        cmd.BindGroup(0, *uniformSlot->page->group, uniformSlot->index * kTextureCount + draw.textureIndex);
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void SkinnedMeshRenderer::Destroy()
{
    DestroyAnimation();
    DestroyPipeline();
    DestroyComputeResources();

    m_uniformPages.clear();
    m_bindLayout.reset();
    m_pendingUploads.reset();
    m_indexBuffer.reset();
    m_worldRenderFrameNumber = std::numeric_limits<std::uint64_t>::max();
    m_worldUniformCursor = 0;
    for (Texture& texture : m_textures)
        texture = {};
    // The shadow map belongs to the terrain renderer; holding it past here leaks it at device teardown.
    m_sunShadow = {};
    m_boundSunShadowTexture = nullptr;
    m_restVertexBuffer.reset();

    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();
    m_indexCount = 0;
    m_targetPass = nullptr;
    m_rhi = nullptr;
    m_assets = nullptr;
}

bool SkinnedMeshRenderer::LoadGltfMesh(const std::string& modelPath)
{
    DestroyAnimation();
    m_importedDiffuseTexturePath.clear();
    if (!m_assets)
        return false;

    // The glTF/GLB asset-import path doesn't generate ozz skeleton/animation sidecars (only
    // FBX did), so a rigged .glb has no _skeleton.ozz and would fail to load as skinned.
    // Generate them on demand here from the glTF via Assimp (LoadGltfMesh below remaps the
    // mesh's bone indices to this ozz skeleton BY JOINT NAME, so the two stay consistent).
    {
        std::filesystem::path gltfPath(modelPath);
        if (gltfPath.is_relative())
        {
            if (auto root = m_assets->RootPath())
                gltfPath = *root / gltfPath;
            else
                gltfPath = std::filesystem::absolute(gltfPath);
        }
        std::error_code canonicalEc;
        const std::filesystem::path canonicalGltf = std::filesystem::weakly_canonical(gltfPath, canonicalEc);
        if (!canonicalEc)
            gltfPath = canonicalGltf;
        const std::filesystem::path stemSkeleton = gltfPath.parent_path() /
            (gltfPath.stem().string() + "_skeleton.ozz");
        const std::filesystem::path legacySkeleton = gltfPath.parent_path() / "skeleton.ozz";
        if (!std::filesystem::exists(stemSkeleton) && !std::filesystem::exists(legacySkeleton))
        {
            AssimpImporter importer;
            const AssimpImporter::ImportResult result = importer.importFile(gltfPath);
            if (result.success && result.skeleton && !result.skeleton->bones.empty())
            {
                std::vector<std::filesystem::path> animationPaths;
                for (std::size_t i = 0; i < result.animations.size(); ++i)
                    animationPaths.push_back(gltfPath.parent_path() /
                        (gltfPath.stem().string() + "_anim_" + std::to_string(i) + ".ozz"));
                std::string ozzError;
                if (!importer.writeOzzSidecars(result, stemSkeleton, animationPaths, ozzError))
                    LogFormat("[GLTF] skinned sidecar generation failed path=%s error=%s",
                        gltfPath.generic_string().c_str(), ozzError.c_str());
                else
                    LogFormat("[GLTF] generated skinned sidecars path=%s animations=%zu",
                        gltfPath.generic_string().c_str(), animationPaths.size());
            }
            else
            {
                LogFormat("[GLTF] skinned sidecar skipped path=%s reason=%s",
                    gltfPath.generic_string().c_str(),
                    result.success ? "no_skeleton" : "import_failed");
            }
        }
    }

    if (!LoadOzzPose(modelPath))
        return false;
    const std::string dir = DirectoryOf(modelPath);

    auto modelBytes = m_assets->ReadAll(modelPath);
    if (!modelBytes)
    {
        LogFormat("[GLTF] failed to read model: %s", modelPath.c_str());
        return false;
    }

    auto data = fastgltf::GltfDataBuffer::FromBytes(
        reinterpret_cast<const std::byte*>(modelBytes->data()), modelBytes->size());
    if (data.error() != fastgltf::Error::None)
    {
        LogFormat("[GLTF] data buffer error for %s: %s", modelPath.c_str(),
            fastgltf::getErrorMessage(data.error()).data());
        return false;
    }

    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
        fastgltf::Options::DecomposeNodeMatrices);
    if (assetResult.error() != fastgltf::Error::None)
    {
        LogFormat("[GLTF] parse error for %s: %s", modelPath.c_str(),
            fastgltf::getErrorMessage(assetResult.error()).data());
        return false;
    }
    fastgltf::Asset asset = std::move(assetResult.get());

    std::unordered_map<std::string, uint32_t> ozzJointByName;
    auto jointNames = m_ozz->skeleton.joint_names();
    for (int i = 0; i < m_ozz->skeleton.num_joints(); ++i)
        ozzJointByName.emplace(jointNames[i], static_cast<uint32_t>(i));

    std::vector<std::vector<uint32_t>> skinJointRemaps(asset.skins.size());
    for (size_t skinIndex = 0; skinIndex < asset.skins.size(); ++skinIndex)
    {
        const auto& skin = asset.skins[skinIndex];
        auto& remaps = skinJointRemaps[skinIndex];
        remaps.assign(skin.joints.size(), 255u);
        for (size_t localJoint = 0; localJoint < skin.joints.size(); ++localJoint)
        {
            const auto& node = asset.nodes[skin.joints[localJoint]];
            auto it = ozzJointByName.find(std::string(node.name));
            if (it != ozzJointByName.end())
                remaps[localJoint] = it->second;
        }

        if (skin.inverseBindMatrices.has_value())
        {
            const auto& accessor = asset.accessors[skin.inverseBindMatrices.value()];
            size_t matrixIndex = 0;
            fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(
                asset, accessor, [&](fastgltf::math::fmat4x4 matrix)
                {
                    if (matrixIndex < remaps.size() && remaps[matrixIndex] < m_inverseBindMatrices.size())
                    {
                        const auto ozzMatrix = ToOzzMatrix(matrix);
                        m_inverseBindMatrices[remaps[matrixIndex]] = ToRowMajorMatrix(ozzMatrix);
                    }
                    ++matrixIndex;
                });
        }
    }

    m_vertices.clear();
    m_indices.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();
    m_draws.clear();

    uint32_t invalidRemappedInfluences = 0;
    uint32_t primitiveIndex = 0;
    int observedIndexBytes = 0;

    for (size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex)
    {
        const auto& node = asset.nodes[nodeIndex];
        if (!node.meshIndex.has_value())
            continue;
        LogFormat("[GLTF] loading skinned mesh '%s' from node '%s' in mesh-local coordinates",
            modelPath.c_str(),
            std::string(node.name).c_str());
        const std::vector<uint32_t>* remaps = nullptr;
        if (node.skinIndex.has_value() && node.skinIndex.value() < skinJointRemaps.size())
            remaps = &skinJointRemaps[node.skinIndex.value()];

        const auto& mesh = asset.meshes[node.meshIndex.value()];
        for (const auto& primitive : mesh.primitives)
        {
            if (primitive.type != fastgltf::PrimitiveType::Triangles)
                continue;

            auto posIt = primitive.findAttribute("POSITION");
            auto normalIt = primitive.findAttribute("NORMAL");
            auto uvIt = primitive.findAttribute("TEXCOORD_0");
            auto jointsIt = primitive.findAttribute("JOINTS_0");
            auto weightsIt = primitive.findAttribute("WEIGHTS_0");
            if (posIt == primitive.attributes.end() || !primitive.indicesAccessor.has_value())
                continue;

            const auto& positionAccessor = asset.accessors[posIt->accessorIndex];
            const size_t vertexCount = positionAccessor.count;
            std::vector<SourceVertex> sourceVertices(vertexCount);
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, positionAccessor, [&](fastgltf::math::fvec3 value, size_t index)
                {
                    sourceVertices[index].position[0] = value.x();
                    sourceVertices[index].position[1] = value.y();
                    sourceVertices[index].position[2] = value.z();
                });

            if (normalIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[normalIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, accessor, [&](fastgltf::math::fvec3 value, size_t index)
                    {
                        sourceVertices[index].normal[0] = value.x();
                        sourceVertices[index].normal[1] = value.y();
                        sourceVertices[index].normal[2] = value.z();
                    });
            }
            if (uvIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[uvIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                    asset, accessor, [&](fastgltf::math::fvec2 value, size_t index)
                    {
                        sourceVertices[index].uv[0] = value.x();
                        sourceVertices[index].uv[1] = value.y();
                    });
            }
            if (jointsIt != primitive.attributes.end() && remaps)
            {
                const auto& accessor = asset.accessors[jointsIt->accessorIndex];
                if (accessor.componentType == fastgltf::ComponentType::UnsignedByte)
                {
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::u8vec4>(
                        asset, accessor, [&](fastgltf::math::u8vec4 value, size_t index)
                        {
                            for (int i = 0; i < 4; ++i)
                            {
                                const uint32_t local = value[i];
                                sourceVertices[index].boneIndices[i] =
                                    local < remaps->size() ? static_cast<uint8_t>((*remaps)[local]) : 255u;
                            }
                        });
                }
                else
                {
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::u16vec4>(
                        asset, accessor, [&](fastgltf::math::u16vec4 value, size_t index)
                        {
                            for (int i = 0; i < 4; ++i)
                            {
                                const uint32_t local = value[i];
                                sourceVertices[index].boneIndices[i] =
                                    local < remaps->size() ? static_cast<uint8_t>((*remaps)[local]) : 255u;
                            }
                        });
                }
            }
            if (weightsIt != primitive.attributes.end())
            {
                const auto& accessor = asset.accessors[weightsIt->accessorIndex];
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                    asset, accessor, [&](fastgltf::math::fvec4 value, size_t index)
                    {
                        sourceVertices[index].boneWeights[0] = ToWeightByte(value.x());
                        sourceVertices[index].boneWeights[1] = ToWeightByte(value.y());
                        sourceVertices[index].boneWeights[2] = ToWeightByte(value.z());
                        sourceVertices[index].boneWeights[3] = ToWeightByte(value.w());
                    });
            }

            const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
            m_vertices.resize(m_vertices.size() + sourceVertices.size());
            RawMesh rawMesh{};
            rawMesh.meshIndex = primitiveIndex++;
            rawMesh.baseVertex = baseVertex;
            rawMesh.vertexCount = static_cast<uint32_t>(sourceVertices.size());
            rawMesh.sourceVertices = std::move(sourceVertices);

            m_restVerticesGpu.reserve(m_restVerticesGpu.size() + rawMesh.sourceVertices.size());
            for (const SourceVertex& source : rawMesh.sourceVertices)
            {
                RestVertexGpu gpu{};
                gpu.position[0] = source.position[0];
                gpu.position[1] = source.position[1];
                gpu.position[2] = source.position[2];
                gpu.normal[0] = source.normal[0];
                gpu.normal[1] = source.normal[1];
                gpu.normal[2] = source.normal[2];
                gpu.uv[0] = source.uv[0];
                gpu.uv[1] = source.uv[1];
                gpu.packedWeights = PackBytes(
                    source.boneWeights[0], source.boneWeights[1],
                    source.boneWeights[2], source.boneWeights[3]);
                gpu.packedBones = PackBytes(
                    source.boneIndices[0], source.boneIndices[1],
                    source.boneIndices[2], source.boneIndices[3]);
                for (uint32_t i = 0; i < 4; ++i)
                {
                    if (source.boneWeights[i] != 0 && source.boneIndices[i] >= m_boneCount)
                        ++invalidRemappedInfluences;
                }
                m_restVerticesGpu.push_back(gpu);
            }

            const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
            const auto& indexAccessor = asset.accessors[primitive.indicesAccessor.value()];
            observedIndexBytes = std::max(observedIndexBytes,
                static_cast<int>(fastgltf::getComponentByteSize(indexAccessor.componentType)));
            fastgltf::iterateAccessor<std::uint32_t>(
                asset, indexAccessor, [&](std::uint32_t index)
                {
                    m_indices.push_back(baseVertex + index);
                });

            MeshDraw draw{};
            draw.firstIndex = firstIndex;
            draw.indexCount = static_cast<uint32_t>(m_indices.size() - firstIndex);
            draw.textureIndex = 0;
            m_draws.push_back(draw);
            LogFormat("[GLTF] extracted primitive[%u] mesh='%s': verts=%zu indices=%u skin=%s",
                rawMesh.meshIndex,
                mesh.name.c_str(),
                rawMesh.sourceVertices.size(),
                draw.indexCount,
                remaps ? "yes" : "no");
            m_rawMeshes.push_back(std::move(rawMesh));
        }
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        Log("[GLTF] no renderable mesh data extracted");
        DestroyAnimation();
        return false;
    }

    if (!SkinPose(0.0f, true, true))
        return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    LogFormat("[MESH] total verts=%zu indices=%zu", m_vertices.size(), m_indices.size());
    LogFormat("[MESH] skinned bbox min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)",
        m_bounds.min[0], m_bounds.min[1], m_bounds.min[2],
        m_bounds.max[0], m_bounds.max[1], m_bounds.max[2]);
    LogFormat("[MESH] skinned bbox center=(%.3f, %.3f, %.3f) displayFitScale=%.3f",
        m_bounds.center[0], m_bounds.center[1], m_bounds.center[2], m_bounds.fitScale);
    LogFormat("[MESH] index size=%d bit, output index buffer=32 bit", observedIndexBytes * 8);
    LogFormat("[COMPUTE] rest-mesh SSBO vertices=%zu stride=%zu invalidRemapSentinels=%u",
        m_restVerticesGpu.size(),
        sizeof(RestVertexGpu),
        invalidRemappedInfluences);
    return true;
}

bool SkinnedMeshRenderer::LoadFbxMesh(const std::string& modelPath)
{
    DestroyAnimation();
    m_importedDiffuseTexturePath.clear();
    if (!m_assets)
        return false;

    std::filesystem::path fbxPath(modelPath);
    if (fbxPath.is_relative())
    {
        if (auto root = m_assets->RootPath())
            fbxPath = *root / fbxPath;
        else
            fbxPath = std::filesystem::absolute(fbxPath);
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(fbxPath, ec);
    if (!ec)
        fbxPath = canonical;

    AssimpImporter importer;
    const AssimpImporter::ImportResult result = importer.importFile(fbxPath);
    if (!result.success)
    {
        LogFormat("[FBX-IMPORT] skinned load failed path=%s error=%s",
            fbxPath.generic_string().c_str(),
            result.errorMessage.c_str());
        return false;
    }
    if (!result.skeleton || result.skeleton->bones.empty())
    {
        LogFormat("[FBX-IMPORT] skinned load skipped path=%s reason=no_skeleton",
            fbxPath.generic_string().c_str());
        return false;
    }

    const std::filesystem::path stemSkeleton = fbxPath.parent_path() /
        (fbxPath.stem().string() + "_skeleton.ozz");
    std::vector<std::filesystem::path> animationPaths;
    for (std::size_t i = 0; i < result.animations.size(); ++i)
        animationPaths.push_back(fbxPath.parent_path() / (fbxPath.stem().string() + "_anim_" + std::to_string(i) + ".ozz"));
    // Sidecars written by an older importer (e.g. before FBX pivot nodes were collapsed) carry a
    // wrong rest pose: the mesh renders scrambled until an animation overrides every bone. They
    // are derived data, so rebuild them from the FBX whenever they no longer match this import.
    const bool sidecarMissing = !std::filesystem::exists(stemSkeleton);
    const bool sidecarStale = !sidecarMissing && !importer.ozzSkeletonSidecarMatches(result, stemSkeleton);
    if (sidecarStale)
    {
        LogFormat("[FBX-IMPORT] stale ozz sidecars (rest pose differs from the FBX) — regenerating path=%s",
            stemSkeleton.generic_string().c_str());
    }
    if (sidecarMissing || sidecarStale)
    {
        std::string ozzError;
        if (!importer.writeOzzSidecars(result, stemSkeleton, animationPaths, ozzError))
        {
            LogFormat("[FBX-IMPORT] skinned sidecar generation failed path=%s error=%s",
                fbxPath.generic_string().c_str(),
                ozzError.c_str());
            return false;
        }
    }

    if (!LoadOzzPose(modelPath))
        return false;

    m_inverseBindMatrices.assign(m_boneCount, IdentityPaletteMatrix());
    for (std::size_t i = 0; i < result.skeleton->bones.size() && i < m_inverseBindMatrices.size(); ++i)
        m_inverseBindMatrices[i] = result.skeleton->bones[i].inverseBindPose;

    m_vertices.clear();
    m_indices.clear();
    m_rawMeshes.clear();
    m_restVerticesGpu.clear();
    m_draws.clear();
    for (const GltfMaterialSource& material : result.materials)
    {
        if (!material.baseColorTexturePath.empty())
        {
            m_importedDiffuseTexturePath = material.baseColorTexturePath;
            break;
        }
    }

    std::unordered_map<int, const AssimpImporter::SkinningData*> skinByMesh;
    for (const AssimpImporter::SkinningData& skin : result.skinning)
        skinByMesh[skin.meshIndex] = &skin;

    uint32_t invalidInfluences = 0;
    uint32_t meshIndex = 0;
    for (std::size_t sourceMeshIndex = 0; sourceMeshIndex < result.meshes.size(); ++sourceMeshIndex)
    {
        const AssimpImporter::StaticMeshData& mesh = result.meshes[sourceMeshIndex];
        if (mesh.vertices.empty() || mesh.indices.empty())
            continue;
        const uint32_t baseVertex = static_cast<uint32_t>(m_vertices.size());
        m_vertices.resize(m_vertices.size() + mesh.vertices.size());

        RawMesh rawMesh{};
        rawMesh.meshIndex = meshIndex++;
        rawMesh.baseVertex = baseVertex;
        rawMesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        rawMesh.sourceVertices.resize(mesh.vertices.size());

        const AssimpImporter::SkinningData* skin = nullptr;
        const auto skinIt = skinByMesh.find(static_cast<int>(sourceMeshIndex));
        if (skinIt != skinByMesh.end())
            skin = skinIt->second;

        for (std::size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex)
        {
            const AssimpImporter::Vertex& source = mesh.vertices[vertexIndex];
            SourceVertex& vertex = rawMesh.sourceVertices[vertexIndex];
            vertex.position[0] = source.position[0];
            vertex.position[1] = source.position[1];
            vertex.position[2] = source.position[2];
            vertex.normal[0] = source.normal[0];
            vertex.normal[1] = source.normal[1];
            vertex.normal[2] = source.normal[2];
            vertex.uv[0] = source.uv[0];
            vertex.uv[1] = source.uv[1];
            if (skin && vertexIndex < skin->influences.size())
            {
                std::memcpy(vertex.boneIndices, skin->influences[vertexIndex].boneIndices, sizeof(vertex.boneIndices));
                std::memcpy(vertex.boneWeights, skin->influences[vertexIndex].boneWeights, sizeof(vertex.boneWeights));
            }
            else
            {
                vertex.boneIndices[0] = 0;
                vertex.boneWeights[0] = 255;
            }

            RestVertexGpu gpu{};
            gpu.position[0] = vertex.position[0];
            gpu.position[1] = vertex.position[1];
            gpu.position[2] = vertex.position[2];
            gpu.normal[0] = vertex.normal[0];
            gpu.normal[1] = vertex.normal[1];
            gpu.normal[2] = vertex.normal[2];
            gpu.uv[0] = vertex.uv[0];
            gpu.uv[1] = vertex.uv[1];
            gpu.packedWeights = PackBytes(vertex.boneWeights[0], vertex.boneWeights[1], vertex.boneWeights[2], vertex.boneWeights[3]);
            gpu.packedBones = PackBytes(vertex.boneIndices[0], vertex.boneIndices[1], vertex.boneIndices[2], vertex.boneIndices[3]);
            for (uint32_t influence = 0; influence < 4; ++influence)
            {
                if (vertex.boneWeights[influence] != 0 && vertex.boneIndices[influence] >= m_boneCount)
                    ++invalidInfluences;
            }
            m_restVerticesGpu.push_back(gpu);
        }

        const uint32_t firstIndex = static_cast<uint32_t>(m_indices.size());
        for (uint32_t index : mesh.indices)
            m_indices.push_back(baseVertex + index);
        MeshDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = static_cast<uint32_t>(m_indices.size() - firstIndex);
        draw.textureIndex = 0;
        m_draws.push_back(draw);
        m_rawMeshes.push_back(std::move(rawMesh));
        LogFormat("[FBX-IMPORT] extracted skinned mesh[%zu] name=%s verts=%zu indices=%u skin=%s",
            sourceMeshIndex,
            mesh.name.c_str(),
            mesh.vertices.size(),
            draw.indexCount,
            skin ? "yes" : "fallback-root");
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        Log("[FBX-IMPORT] no renderable skinned FBX mesh data extracted");
        DestroyAnimation();
        return false;
    }

    if (!SkinPose(0.0f, true, true))
        return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    LogFormat("[FBX-IMPORT] skinned loaded path=%s verts=%zu indices=%zu bones=%u animations=%zu invalidInfluences=%u",
        fbxPath.generic_string().c_str(),
        m_vertices.size(),
        m_indices.size(),
        m_boneCount,
        result.animations.size(),
        invalidInfluences);
    return true;
}

bool SkinnedMeshRenderer::LoadOzzPose(const std::string& modelPath)
{
    if (!m_assets)
        return false;

    m_ozz = std::make_unique<OzzRuntime>();
    const std::string dir = DirectoryOf(modelPath);
    const std::string stem = std::filesystem::path(modelPath).stem().string();
    const std::string stemSkeletonPath = dir + "/" + stem + "_skeleton.ozz";
    const std::string legacySkeletonPath = dir + "/skeleton.ozz";
    const std::string skeletonPath = m_assets->ReadAll(stemSkeletonPath).has_value()
        ? stemSkeletonPath
        : legacySkeletonPath;
    if (!ReadOzzObject(*m_assets, skeletonPath, m_ozz->skeleton))
    {
        DestroyAnimation();
        return false;
    }

    m_boneCount = static_cast<uint32_t>(m_ozz->skeleton.num_joints());
    m_inverseBindMatrices.assign(m_boneCount, IdentityPaletteMatrix());
    m_bonePaletteCpu.assign(m_boneCount, IdentityPaletteMatrix());
    m_ozz->locals.resize(static_cast<size_t>(m_ozz->skeleton.num_soa_joints()));
    m_ozz->models.resize(static_cast<size_t>(m_ozz->skeleton.num_joints()));
    m_ozz->context.Resize(m_ozz->skeleton.num_joints());

    // Load up to three motion clips: idle (_anim_0), walk (_anim_1), run (_anim_2). Each is
    // optional; a missing/incompatible clip is simply skipped and the renderer falls back to
    // idle (then rest pose) at sample time. This lets walk/run light up automatically once a
    // character model with multiple animation clips is imported.
    auto loadClip = [&](const std::string& stemSuffix, const std::string& legacyName,
                        ozz::animation::Animation& anim) -> bool {
        const std::string stemPath = dir + "/" + stem + stemSuffix;
        const std::string legacyPath = dir + "/" + legacyName;
        const std::string path = m_assets->ReadAll(stemPath).has_value() ? stemPath : legacyPath;
        if (ReadOzzObject(*m_assets, path, anim) &&
            anim.num_tracks() == m_ozz->skeleton.num_joints())
        {
            LogFormat("[OZZ] loaded clip=%s duration=%.3f", path.c_str(), anim.duration());
            return true;
        }
        return false;
    };
    m_ozz->hasIdle = loadClip("_anim_0.ozz", "idle.ozz", m_ozz->idle);
    m_ozz->hasWalk = loadClip("_anim_1.ozz", "walk.ozz", m_ozz->walk);
    m_ozz->hasRun = loadClip("_anim_2.ozz", "run.ozz", m_ozz->run);
    LogFormat("[OZZ] loaded skeleton=%s bones=%u clips: idle=%s walk=%s run=%s",
        skeletonPath.c_str(), m_boneCount,
        m_ozz->hasIdle ? "yes" : "no",
        m_ozz->hasWalk ? "yes" : "no",
        m_ozz->hasRun ? "yes" : "no");
    return true;
}

void SkinnedMeshRenderer::SetMotionState(MotionState state)
{
    const bool changed = m_motionState != state;
    m_motionState = state;
    if (changed)
        LogFormat("[ANIM-PREVIEW] state=%s", MotionStateName(state));
}

float SkinnedMeshRenderer::GroundOffsetY() const
{
    return -m_bounds.min[1];
}

bool SkinnedMeshRenderer::SamplePoseFromState(float animTimeSeconds, MotionState state,
    ozz::span<ozz::math::SoaTransform> outLocals)
{
    if (!m_ozz)
        return false;
    return SamplePose(animTimeSeconds, state, m_ozz->context, outLocals);
}

bool SkinnedMeshRenderer::SamplePose(float animTimeSeconds,
    MotionState state,
    ozz::animation::SamplingJob::Context& context,
    ozz::span<ozz::math::SoaTransform> outLocals) const
{
    if (!m_ozz)
        return false;

    // Select the clip for the requested motion state, falling back to idle (then the rest
    // pose) when the requested clip isn't loaded.
    const ozz::animation::Animation* clip = nullptr;
    if (state == MotionState::Walk && m_ozz->hasWalk)
        clip = &m_ozz->walk;
    else if (state == MotionState::Run && m_ozz->hasRun)
        clip = &m_ozz->run;
    else if (m_ozz->hasIdle)
        clip = &m_ozz->idle;

    if (clip)
    {
        const float duration = clip->duration();
        const float ratio = duration > 0.0f
            ? std::fmod(std::max(animTimeSeconds, 0.0f), duration) / duration
            : 0.0f;
        ozz::animation::SamplingJob samplingJob;
        samplingJob.animation = clip;
        samplingJob.context = &context;
        samplingJob.ratio = ratio;
        samplingJob.output = outLocals;
        if (!samplingJob.Run())
            return false;
    }
    else
    {
        const auto rest = m_ozz->skeleton.joint_rest_poses();
        std::copy(rest.begin(), rest.end(), outLocals.begin());
    }
    return true;
}

bool SkinnedMeshRenderer::BuildPaletteFromLocals(ozz::span<const ozz::math::SoaTransform> locals)
{
    if (!m_ozz)
        return false;
    return BuildPalette(locals, ozz::make_span(m_ozz->models), m_bonePaletteCpu);
}

bool SkinnedMeshRenderer::BuildPalette(ozz::span<const ozz::math::SoaTransform> locals,
    ozz::span<ozz::math::Float4x4> models,
    std::vector<ixtreeme::math::Mat4>& palette) const
{
    if (!m_ozz || m_boneCount == 0 || palette.size() < m_boneCount)
        return false;

    ozz::animation::LocalToModelJob localToModel;
    localToModel.skeleton = &m_ozz->skeleton;
    localToModel.input = locals;
    localToModel.output = models;
    if (!localToModel.Run())
        return false;

    for (uint32_t bone = 0; bone < m_boneCount; ++bone)
    {
        ozz::math::Float4x4 inverseBind = ozz::math::Float4x4::identity();
        for (int col = 0; col < 4; ++col)
        {
            inverseBind.cols[col] = ozz::math::simd_float4::Load(
                m_inverseBindMatrices[bone].m[0 * 4 + col],
                m_inverseBindMatrices[bone].m[1 * 4 + col],
                m_inverseBindMatrices[bone].m[2 * 4 + col],
                m_inverseBindMatrices[bone].m[3 * 4 + col]);
        }
        palette[bone] = ToRowVectorPaletteMatrix(models[bone] * inverseBind);
    }
    return true;
}

void SkinnedMeshRenderer::ReserveParallelPalettes(uint32_t threads)
{
    if (!m_ozz)
        return;
    while (m_poseScratch.size() < threads)
    {
        auto scratch = std::make_unique<PoseScratch>();
        scratch->context.Resize(m_ozz->skeleton.num_joints());
        scratch->locals.resize(static_cast<size_t>(m_ozz->skeleton.num_soa_joints()));
        scratch->models.resize(static_cast<size_t>(m_ozz->skeleton.num_joints()));
        scratch->palette.assign(m_boneCount, IdentityPaletteMatrix());
        m_poseScratch.push_back(std::move(scratch));
    }
}

bool SkinnedMeshRenderer::PreparePalette(const ixrhi::IXRHIFrameInfo& frame,
    const uint32_t* skinSlots,
    uint32_t slotCount,
    ozz::span<const ozz::math::SoaTransform> pose,
    MotionState state,
    float animTimeSeconds)
{
    const uint32_t worker = ixjobs::JobSystem::CurrentWorker();
    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (!m_ozz || m_boneCount == 0 || worker >= m_poseScratch.size() || !frame.frameActive)
        return false;
    PoseScratch& scratch = *m_poseScratch[worker];
    ozz::span<const ozz::math::SoaTransform> locals = pose;
    if (locals.empty())
    {
        if (!SamplePose(animTimeSeconds, state, scratch.context, ozz::make_span(scratch.locals)))
            return false;
        locals = ozz::make_span(scratch.locals);
    }
    else if (locals.size() < NumSoaJoints())
    {
        return false;  // a pose for another skeleton
    }
    if (!BuildPalette(locals, ozz::make_span(scratch.models), scratch.palette))
        return false;
    const std::size_t bytes = sizeof(Mat4) * m_boneCount;
    bool written = false;
    for (uint32_t i = 0; i < slotCount; ++i)
    {
        if (ixrhi::IXRHIBuffer* palette = BonePalette(frameIndex, skinSlots[i]))
        {
            palette->Write(0, scratch.palette.data(), bytes);
            written = true;
        }
    }
    return written;
}

void SkinnedMeshRenderer::RecordSkins(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    const uint32_t* skinSlots,
    uint32_t slotCount,
    bool withoutBarrier)
{
    if (!m_computePipeline || !frame.frameActive || slotCount == 0)
        return;
    RecordPendingUploads(cmd, frame);
    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    cmd.SetComputePipeline(*m_computePipeline);
    SkinPushConstants push{};
    push.vertexCount = static_cast<uint32_t>(m_vertices.size());
    push.boneCount = m_boneCount;
    const uint32_t groupCount = (push.vertexCount + 63u) / 64u;
    for (uint32_t i = 0; i < slotCount; ++i)
    {
        const uint32_t slot = skinSlots[i];
        if (!SkinnedOutput(frameIndex, slot))
            continue;
        cmd.BindGroup(0, *m_skinPages[slot / kSkinSlots]->computeGroup, frameIndex * kSkinSlots + slot % kSkinSlots);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Dispatch(groupCount, 1, 1);
        if (!withoutBarrier)
            EmitSkinBarrier(cmd, frameIndex, slot);
    }
}

void SkinnedMeshRenderer::RenderShadowCasters(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    const WorldMat4& lightViewProj,
    const ixrhi::IXRHIRenderPass* shadowPass,
    const ShadowCasterInstance* instances,
    std::size_t count)
{
    if (!m_rhi || !m_indexBuffer || m_indexCount == 0 || !shadowPass || !frame.frameActive || count == 0)
        return;
    if (m_shadowPass != shadowPass)
    {
        m_shadowPipeline.reset();
        m_shadowPipelineFailed = false;
        m_shadowPass = shadowPass;
    }
    if (!m_shadowPipeline && !m_shadowPipelineFailed && !CreateShadowPipeline(*m_rhi, shadowPass))
    {
        m_shadowPipelineFailed = true;
        Log("[MESH] skinned mesh sun shadow pipeline could not be created");
    }
    if (!m_shadowPipeline)
        return;
    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    const Mat4 lightViewProjLocal = ToLocalMat4(lightViewProj);
    cmd.SetGraphicsPipeline(*m_shadowPipeline);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
    for (std::size_t i = 0; i < count; ++i)
    {
        const ShadowCasterInstance& instance = instances[i];
        ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, instance.skinSlot);
        if (!skinned)
            continue;
        // The same placement as the lit draws (UpdateWorldUniform), into the cascade's light space.
        const Mat4 model = Multiply(Multiply(xm::Scale({instance.scale[0], instance.scale[1], instance.scale[2]}),
                                        RotationY(-instance.yawRadians)),
            Translation(instance.position.x, instance.position.y, instance.position.z));
        const Mat4 mvp = Multiply(model, lightViewProjLocal);
        cmd.PushConstants(&mvp, sizeof(mvp));
        cmd.SetVertexBuffer(0, *skinned, 0);
        for (const MeshDraw& draw : m_draws)
            cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void SkinnedMeshRenderer::RecordSkin(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame, uint32_t skinSlot)
{
    if (!m_computePipeline || !frame.frameActive || !SkinnedOutput(frame.frameIndex % kFramesInFlight, skinSlot))
        return;
    RecordPendingUploads(cmd, frame);
    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    DispatchSkin(cmd, frameIndex, skinSlot);
    EmitSkinBarrier(cmd, frameIndex, skinSlot);
}

bool SkinnedMeshRenderer::CpuSkinVertices(bool updateBounds, bool logSamples)
{
    float minValue[3] = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    float maxValue[3] = {
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};

    int sampleLogs = 0;
    for (const RawMesh& rawMesh : m_rawMeshes)
    {
        for (uint32_t vertexIndex = 0; vertexIndex < rawMesh.vertexCount; ++vertexIndex)
        {
            const SourceVertex& source = rawMesh.sourceVertices[vertexIndex];
            Vertex out{};
            int modelBones[4]{};
            SkinVertex(source, m_bonePaletteCpu, out.position, out.normal, out.uv, modelBones);

            const uint32_t outputIndex = rawMesh.baseVertex + vertexIndex;
            if (outputIndex >= m_vertices.size())
                return false;
            m_vertices[outputIndex] = out;

            if (updateBounds)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    minValue[axis] = std::min(minValue[axis], out.position[axis]);
                    maxValue[axis] = std::max(maxValue[axis], out.position[axis]);
                }
            }

            if (logSamples && sampleLogs < 2)
            {
                LogFormat("[SKIN] sample mesh[%u] vertex[%u]: rawPos=(%.3f, %.3f, %.3f) skinnedPos=(%.3f, %.3f, %.3f) weights=(%u,%u,%u,%u) localBones=(%u,%u,%u,%u) modelBones=(%d,%d,%d,%d)",
                    rawMesh.meshIndex,
                    vertexIndex,
                    source.position[0], source.position[1], source.position[2],
                    out.position[0], out.position[1], out.position[2],
                    source.boneWeights[0], source.boneWeights[1], source.boneWeights[2], source.boneWeights[3],
                    source.boneIndices[0], source.boneIndices[1], source.boneIndices[2], source.boneIndices[3],
                    modelBones[0], modelBones[1], modelBones[2], modelBones[3]);
                ++sampleLogs;
            }
        }

        if (logSamples)
        {
            LogFormat("[SKIN] mesh[%u]: skinned %u verts, bones=%u",
                rawMesh.meshIndex,
                rawMesh.vertexCount,
                m_boneCount);
        }
    }

    if (updateBounds)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            m_bounds.min[axis] = minValue[axis];
            m_bounds.max[axis] = maxValue[axis];
            m_bounds.center[axis] = (minValue[axis] + maxValue[axis]) * 0.5f;
        }

        const float sizeX = m_bounds.max[0] - m_bounds.min[0];
        const float sizeY = m_bounds.max[1] - m_bounds.min[1];
        const float sizeZ = m_bounds.max[2] - m_bounds.min[2];
        const float maxDimension = std::max(sizeX, std::max(sizeY, sizeZ));
        m_bounds.fitScale = maxDimension > 0.0001f ? (2.35f / maxDimension) : 1.0f;
    }

    return true;
}

bool SkinnedMeshRenderer::SkinPose(float animTimeSeconds, bool updateBounds, bool logSamples, MotionState state)
{
    if (!m_ozz || m_boneCount == 0)
        return false;

    // Full path (load-time bounds + the one-time compute-skin verification): sample the clip,
    // build the GPU palette, AND CPU-skin every vertex. The CPU vertex loop populates
    // m_vertices, which VerifyComputeSkin compares against the GPU result and which the bounds
    // pass reads — so it must always run here. The per-frame runtime path goes through
    // UploadBonePalette, which deliberately skips this loop.
    if (!SamplePoseFromState(animTimeSeconds, state, ozz::make_span(m_ozz->locals)))
        return false;
    if (!BuildPaletteFromLocals(ozz::make_span(m_ozz->locals)))
        return false;
    return CpuSkinVertices(updateBounds, logSamples);
}

bool SkinnedMeshRenderer::UploadPaletteToBuffer(uint32_t frameIndex, uint32_t skinSlot)
{
    ixrhi::IXRHIBuffer* palette = BonePalette(frameIndex, skinSlot);
    if (!palette || m_bonePaletteCpu.empty())
        return false;

    const std::size_t size = sizeof(Mat4) * m_bonePaletteCpu.size();
    palette->Write(0, m_bonePaletteCpu.data(), size);
    return true;
}

bool SkinnedMeshRenderer::UploadBonePalette(float animTimeSeconds, uint32_t frameIndex)
{
    return UploadBonePalette(m_motionState, animTimeSeconds, frameIndex, 0);
}

bool SkinnedMeshRenderer::UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot)
{
    if (!BonePalette(frameIndex, skinSlot) || !m_ozz || m_bonePaletteCpu.empty())
    {
        return false;
    }

    // Runtime per-frame path: sample the clip + build the GPU palette, but SKIP the CPU
    // vertex-skinning loop (it only feeds bounds/verification, which are done at load).
    if (!SamplePoseFromState(animTimeSeconds, state, ozz::make_span(m_ozz->locals)))
        return false;
    if (!BuildPaletteFromLocals(ozz::make_span(m_ozz->locals)))
        return false;
    return UploadPaletteToBuffer(frameIndex, skinSlot);
}

void SkinnedMeshRenderer::RecordPendingUploads(ixrhi::IXRHICommandList& cmd, const ixrhi::IXRHIFrameInfo& frame)
{
    if (!m_pendingUploads || !frame.frameActive)
        return;
    PendingUploads& uploads = *m_pendingUploads;
    if (uploads.recordedFrame != std::numeric_limits<std::uint64_t>::max())
    {
        // The frame that copied them is done once its slot comes round again.
        if (frame.frameNumber >= uploads.recordedFrame + kFramesInFlight)
            m_pendingUploads.reset();
        return;
    }
    uploads.recordedFrame = frame.frameNumber;
    if (uploads.indexStaging && m_indexBuffer)
    {
        cmd.CopyBuffer(*uploads.indexStaging, *m_indexBuffer, uploads.indexStaging->SizeBytes());
        cmd.TransitionBuffer(*m_indexBuffer, ixrhi::IXRHIBufferState::TransferDst, ixrhi::IXRHIBufferState::IndexRead);
    }
    if (uploads.restStaging && m_restVertexBuffer)
    {
        cmd.CopyBuffer(*uploads.restStaging, *m_restVertexBuffer, uploads.restStaging->SizeBytes());
        cmd.TransitionBuffer(*m_restVertexBuffer, ixrhi::IXRHIBufferState::TransferDst,
            ixrhi::IXRHIBufferState::ShaderRead);
    }
    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        const std::shared_ptr<ixrhi::IXRHIBuffer>& staging = uploads.textureStaging[textureIndex];
        Texture& texture = m_textures[textureIndex];
        if (!staging || !texture.image)
            continue;
        cmd.TransitionTexture(*texture.image, ixrhi::IXRHIImageLayout::Undefined, ixrhi::IXRHIImageLayout::TransferDst);
        cmd.CopyBufferToTexture(*staging, 0, texture.width, *texture.image, 0, 0, 0, 0, texture.width, texture.height);
        cmd.TransitionTexture(*texture.image, ixrhi::IXRHIImageLayout::TransferDst,
            ixrhi::IXRHIImageLayout::ShaderReadOnly);
    }
}

void SkinnedMeshRenderer::DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex)
{
    DispatchSkin(cmd, frameIndex, 0);
}

void SkinnedMeshRenderer::DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex, uint32_t skinSlot)
{
    if (frameIndex >= kFramesInFlight || skinSlot / kSkinSlots >= m_skinPages.size())
        return;
    cmd.SetComputePipeline(*m_computePipeline);
    cmd.BindGroup(0, *m_skinPages[skinSlot / kSkinSlots]->computeGroup, frameIndex * kSkinSlots + skinSlot % kSkinSlots);

    SkinPushConstants push{};
    push.vertexCount = static_cast<uint32_t>(m_vertices.size());
    push.boneCount = m_boneCount;
    cmd.PushConstants(&push, sizeof(push));

    const uint32_t groupCount = (push.vertexCount + 63u) / 64u;
    cmd.Dispatch(groupCount, 1, 1);
}

void SkinnedMeshRenderer::EmitSkinBarrier(ixrhi::IXRHICommandList& cmd,
                                          uint32_t frameIndex,
                                          uint32_t skinSlot)
{
    // ComputeShaderWrite -> VertexInputRead, exactly like the pre-migration
    // native buffer barrier (same stages/access, whole output buffer).
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, skinSlot);
    if (!skinned)
        return;
    cmd.TransitionBuffer(*skinned,
        ixrhi::IXRHIBufferState::ShaderWrite,
        ixrhi::IXRHIBufferState::VertexRead);
}

bool SkinnedMeshRenderer::VerifyComputeSkin(ixrhi::IXRHIDevice& rhi)
{
    constexpr float verifyTime = 0.5f;
    if (!SkinPose(verifyTime, false, false))
        return false;
    const std::vector<Vertex> cpuVertices = m_vertices;

    if (!UploadBonePalette(verifyTime, 0))
        return false;

    const std::size_t vertexSize = sizeof(Vertex) * cpuVertices.size();
    ixrhi::IXRHIBufferDesc stagingDesc;
    stagingDesc.sizeBytes = vertexSize;
    stagingDesc.usage = ixrhi::IXRHIBufferUsage::TransferDst;
    stagingDesc.cpuAccess = ixrhi::IXRHICpuAccess::Write;
    stagingDesc.debugName = "SkinnedMesh:VerifyStaging";
    auto staging = rhi.CreateBuffer(stagingDesc, nullptr, 0);
    if (!staging)
        return false;

    // Same dispatch + compute->transfer barrier + copy as the old one-time
    // submit path, expressed through public IXRHI primitives and executed
    // synchronously by the backend.
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(0, 0);
    if (!skinned)
        return false;
    rhi.ExecuteAndWait([&](ixrhi::IXRHICommandList& cmd) {
        DispatchSkin(cmd, 0);
        cmd.TransitionBuffer(*skinned,
            ixrhi::IXRHIBufferState::ShaderWrite,
            ixrhi::IXRHIBufferState::TransferSrc);
        cmd.CopyBuffer(*skinned, *staging, vertexSize);
    });

    std::vector<Vertex> gpuVertices(cpuVertices.size());
    staging->Read(0, gpuVertices.data(), vertexSize);

    float maxPosDelta = 0.0f;
    float maxNormalDelta = 0.0f;
    uint32_t withinCount = 0;
    uint32_t offendingLogged = 0;
    for (size_t i = 0; i < cpuVertices.size(); ++i)
    {
        float vertexPosDelta = 0.0f;
        float vertexNormalDelta = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            vertexPosDelta = std::max(vertexPosDelta, xm::Abs(cpuVertices[i].position[axis] - gpuVertices[i].position[axis]));
            vertexNormalDelta = std::max(vertexNormalDelta, xm::Abs(cpuVertices[i].normal[axis] - gpuVertices[i].normal[axis]));
        }
        maxPosDelta = std::max(maxPosDelta, vertexPosDelta);
        maxNormalDelta = std::max(maxNormalDelta, vertexNormalDelta);
        if (vertexPosDelta < 0.01f && vertexNormalDelta < 0.001f)
            ++withinCount;
        else if (offendingLogged < 5)
        {
            const RestVertexGpu& rest = m_restVerticesGpu[i];
            LogFormat("[SKIN-VERIFY] mismatch vertex=%zu cpuPos=(%.6f,%.6f,%.6f) gpuPos=(%.6f,%.6f,%.6f) posDelta=%.6f cpuN=(%.6f,%.6f,%.6f) gpuN=(%.6f,%.6f,%.6f) nrmDelta=%.6f weights=0x%08x bones=0x%08x",
                i,
                cpuVertices[i].position[0], cpuVertices[i].position[1], cpuVertices[i].position[2],
                gpuVertices[i].position[0], gpuVertices[i].position[1], gpuVertices[i].position[2],
                vertexPosDelta,
                cpuVertices[i].normal[0], cpuVertices[i].normal[1], cpuVertices[i].normal[2],
                gpuVertices[i].normal[0], gpuVertices[i].normal[1], gpuVertices[i].normal[2],
                vertexNormalDelta,
                rest.packedWeights,
                rest.packedBones);
            ++offendingLogged;
        }
    }

    LogFormat("[SKIN-VERIFY] verts=%zu maxPosDelta=%.6f cm maxNormalDelta=%.6f within(pos<0.01,nrm<0.001)=%u/%zu",
        cpuVertices.size(),
        maxPosDelta,
        maxNormalDelta,
        withinCount,
        cpuVertices.size());

    if (maxPosDelta >= 0.01f || maxNormalDelta >= 0.001f)
    {
        Log("[SKIN-VERIFY] FAILED: GPU compute skinning differs from CPU reference");
        return false;
    }
    return true;
}

bool SkinnedMeshRenderer::CreateBuffers(ixrhi::IXRHIDevice& rhi)
{
    // Device-local: written once, read by every view's and the reflection's draws.
    const std::uint64_t bytes = sizeof(uint32_t) * m_indices.size();
    if (!m_deferUploads)
    {
        m_indexBuffer = CreateRhiBuffer(rhi,
            bytes,
            ixrhi::IXRHIBufferUsage::Index,
            ixrhi::IXRHICpuAccess::None,
            m_indices.data(),
            "SkinnedMesh:IB");
        return m_indexBuffer != nullptr;
    }
    m_pendingUploads = std::make_unique<PendingUploads>();
    m_indexBuffer = CreateRhiBuffer(rhi,
        bytes,
        ixrhi::IXRHIBufferUsage::Index | ixrhi::IXRHIBufferUsage::TransferDst,
        ixrhi::IXRHICpuAccess::None,
        nullptr,
        "SkinnedMesh:IB");
    m_pendingUploads->indexStaging = CreateRhiBuffer(rhi,
        bytes,
        ixrhi::IXRHIBufferUsage::TransferSrc,
        ixrhi::IXRHICpuAccess::Write,
        m_indices.data(),
        "SkinnedMesh:IBStaging");
    return m_indexBuffer != nullptr && m_pendingUploads->indexStaging != nullptr;
}

bool SkinnedMeshRenderer::CreateComputeResources(ixrhi::IXRHIDevice& rhi)
{
    const std::uint64_t restSize = sizeof(RestVertexGpu) * m_restVerticesGpu.size();
    const std::uint64_t vertexSize = sizeof(Vertex) * m_vertices.size();
    const std::uint64_t paletteSize = sizeof(Mat4) * static_cast<size_t>(m_boneCount);
    if (restSize == 0 || vertexSize == 0 || paletteSize == 0)
    {
        LogFormat("[COMPUTE] invalid buffer sizes rest=%llu output=%llu palette=%llu",
            static_cast<unsigned long long>(restSize),
            static_cast<unsigned long long>(vertexSize),
            static_cast<unsigned long long>(paletteSize));
        return false;
    }

    if (m_deferUploads && m_pendingUploads)
    {
        m_restVertexBuffer = CreateRhiBuffer(rhi,
            restSize,
            ixrhi::IXRHIBufferUsage::Storage | ixrhi::IXRHIBufferUsage::TransferDst,
            ixrhi::IXRHICpuAccess::None,
            nullptr,
            "SkinnedMesh:RestVertices");
        m_pendingUploads->restStaging = CreateRhiBuffer(rhi,
            restSize,
            ixrhi::IXRHIBufferUsage::TransferSrc,
            ixrhi::IXRHICpuAccess::Write,
            m_restVerticesGpu.data(),
            "SkinnedMesh:RestStaging");
        if (!m_pendingUploads->restStaging)
            return false;
    }
    else
    {
        m_restVertexBuffer = CreateRhiBuffer(rhi,
            restSize,
            ixrhi::IXRHIBufferUsage::Storage,
            ixrhi::IXRHICpuAccess::None,
            m_restVerticesGpu.data(),
            "SkinnedMesh:RestVertices");
    }
    if (!m_restVertexBuffer)
        return false;

    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
        {1, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
        {2, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
    };
    m_computeBindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_computeBindLayout)
        return false;
    m_skinPages.clear();
    m_loggedSkinPagesFull = false;
    if (!AddSkinPage(rhi))
        return false;
    if (!CreateComputePipeline(rhi))
        return false;
    if (!m_deferUploads && (!EnsureSkinSlot(0) || !VerifyComputeSkin(rhi)))
        return false;

    LogFormat("[COMPUTE] resources OK rest=%zu bytes output/frame/slot=%zu bytes palette/frame/slot=%zu bytes slots=%u (up to %u)",
        static_cast<size_t>(restSize),
        static_cast<size_t>(vertexSize),
        static_cast<size_t>(paletteSize),
        kSkinSlots,
        MaxSkinSlots());
    return true;
}

SkinnedMeshRenderer::SkinPage* SkinnedMeshRenderer::AddSkinPage(ixrhi::IXRHIDevice& rhi)
{
    if (!m_computeBindLayout || !m_restVertexBuffer || m_skinPages.size() >= kMaxSkinPages)
        return nullptr;
    const std::uint64_t restSize = sizeof(RestVertexGpu) * m_restVerticesGpu.size();
    const std::uint64_t vertexSize = sizeof(Vertex) * m_vertices.size();
    const std::uint64_t paletteSize = sizeof(Mat4) * static_cast<size_t>(m_boneCount);
    (void)restSize;
    (void)vertexSize;
    (void)paletteSize;
    auto page = std::make_unique<SkinPage>();
    page->computeGroup = rhi.CreateBindGroup(*m_computeBindLayout, kFramesInFlight * kSkinSlots);
    if (!page->computeGroup)
        return nullptr;
    m_skinPages.push_back(std::move(page));
    return m_skinPages.back().get();
}

bool SkinnedMeshRenderer::EnsureSkinSlot(uint32_t skinSlot)
{
    if (skinSlot >= MaxSkinSlots() || !m_rhi)
        return false;
    // A page made mid-frame is new: nothing recorded so far uses its buffers or sets.
    while (skinSlot / kSkinSlots >= m_skinPages.size())
    {
        if (!AddSkinPage(*m_rhi))
        {
            if (!m_loggedSkinPagesFull)
            {
                LogFormat("[COMPUTE] skin slot %u could not be made (%zu pages)", skinSlot, m_skinPages.size());
                m_loggedSkinPagesFull = true;
            }
            return false;
        }
    }
    // The slot's own buffers (both frames), when it is first used: its sets were never bound.
    SkinPage& page = *m_skinPages[skinSlot / kSkinSlots];
    const uint32_t slot = skinSlot % kSkinSlots;
    if (page.outputs[0][slot] && page.outputs[kFramesInFlight - 1][slot])
        return true;
    const std::uint64_t restSize = sizeof(RestVertexGpu) * m_restVerticesGpu.size();
    const std::uint64_t vertexSize = sizeof(Vertex) * m_vertices.size();
    const std::uint64_t paletteSize = sizeof(Mat4) * static_cast<size_t>(m_boneCount);
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        std::shared_ptr<ixrhi::IXRHIBuffer> palette = CreateRhiBuffer(*m_rhi,
            paletteSize,
            ixrhi::IXRHIBufferUsage::Storage,
            ixrhi::IXRHICpuAccess::Write,
            nullptr,
            "SkinnedMesh:BonePalette");
        // Skinned output is written by compute and read as vertex input
        // (plus transfer-source for verification readback).
        std::shared_ptr<ixrhi::IXRHIBuffer> output = CreateRhiBuffer(*m_rhi,
            vertexSize,
            ixrhi::IXRHIBufferUsage::Storage | ixrhi::IXRHIBufferUsage::Vertex |
                ixrhi::IXRHIBufferUsage::TransferSrc,
            ixrhi::IXRHICpuAccess::None,
            nullptr,
            "SkinnedMesh:SkinnedOutput");
        if (!palette || !output)
            return false;
        const uint32_t set = frame * kSkinSlots + slot;
        page.computeGroup->UpdateBuffer(set, 0, m_restVertexBuffer, 0, restSize);
        page.computeGroup->UpdateBuffer(set, 1, palette, 0, paletteSize);
        page.computeGroup->UpdateBuffer(set, 2, output, 0, vertexSize);
        page.palettes[frame][slot] = std::move(palette);
        page.outputs[frame][slot] = std::move(output);
    }
    return true;
}

ixrhi::IXRHIBuffer* SkinnedMeshRenderer::SkinnedOutput(uint32_t frameIndex, uint32_t skinSlot) const
{
    if (frameIndex >= kFramesInFlight || skinSlot / kSkinSlots >= m_skinPages.size())
        return nullptr;
    return m_skinPages[skinSlot / kSkinSlots]->outputs[frameIndex][skinSlot % kSkinSlots].get();
}

ixrhi::IXRHIBuffer* SkinnedMeshRenderer::BonePalette(uint32_t frameIndex, uint32_t skinSlot) const
{
    if (frameIndex >= kFramesInFlight || skinSlot / kSkinSlots >= m_skinPages.size())
        return nullptr;
    return m_skinPages[skinSlot / kSkinSlots]->palettes[frameIndex][skinSlot % kSkinSlots].get();
}

struct SkinnedMeshRenderer::DecodedTextures
{
    std::array<DdsImage, SkinnedMeshRenderer::kTextureCount> images;
};

void SkinnedMeshRenderer::DecodeTextures(const std::string& modelPath)
{
    const std::array<std::string, kTextureCount> textureFiles = {
        modelPath + "#baseColor",
        modelPath + "#fallback"};
    m_decodedTextures = std::make_unique<DecodedTextures>();
    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        DdsImage& dds = m_decodedTextures->images[textureIndex];
        bool loaded = false;
        if (textureIndex == 0)
        {
            if (!m_importedDiffuseTexturePath.empty())
                loaded = LoadRgbaTextureFile(m_importedDiffuseTexturePath, dds);
            if (!loaded && m_assets)
                loaded = LoadGltfBaseColorTexture(*m_assets, modelPath, dds);
        }
        if (!loaded)
        {
            LogFormat("[DDS] Falling back to 4x4 white RGBA8888 texture for %s",
                textureFiles[textureIndex].c_str());
            dds = CreateFallbackWhiteDdsImage(textureFiles[textureIndex]);
        }
    }
}

bool SkinnedMeshRenderer::UploadDecodedTextures(ixrhi::IXRHIDevice& rhi)
{
    if (!m_decodedTextures)
        return false;
    const ixrhi::IXRHITextureUsage sampledUpload =
        ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    const ixrhi::IXRHICapabilities& caps = rhi.GetCapabilities();
    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        DdsImage& dds = m_decodedTextures->images[textureIndex];
        if (!rhi.IsTextureFormatSupported(dds.format, sampledUpload))
        {
            LogFormat("[DDS] unsupported format features for %s format=%s",
                dds.filename.c_str(),
                RhiFormatName(dds.format));
            LogFormat("[DDS] Falling back to 4x4 white RGBA8888 texture for %s",
                dds.filename.c_str());
            dds = CreateFallbackWhiteDdsImage(dds.filename);
            if (!rhi.IsTextureFormatSupported(dds.format, sampledUpload))
            {
                LogFormat("[DDS] fallback texture format unsupported for %s format=%s",
                    dds.filename.c_str(),
                    RhiFormatName(dds.format));
                return false;
            }
        }

        Texture& texture = m_textures[textureIndex];
        texture.name = dds.filename;
        texture.width = dds.width;
        texture.height = dds.height;
        texture.mipLevels = dds.mipLevels;
        texture.format = dds.format;

        ixrhi::IXRHITextureDesc imageDesc;
        imageDesc.width = dds.width;
        imageDesc.height = dds.height;
        imageDesc.format = dds.format;
        imageDesc.usage = sampledUpload;
        imageDesc.debugName = "SkinnedMesh:" + dds.filename;
        if (m_deferUploads && m_pendingUploads)
        {
            // Made empty; its texels (mip 0, all an image of mipLevels 1 takes) from staging.
            texture.image = rhi.CreateTexture(imageDesc, nullptr, 0);
            m_pendingUploads->textureStaging[textureIndex] = CreateRhiBuffer(rhi,
                dds.pixels.size(),
                ixrhi::IXRHIBufferUsage::TransferSrc,
                ixrhi::IXRHICpuAccess::Write,
                dds.pixels.data(),
                ("SkinnedMesh:" + dds.filename + ":Staging").c_str());
            if (!m_pendingUploads->textureStaging[textureIndex])
                return false;
        }
        else
        {
            texture.image = rhi.CreateTexture(imageDesc, dds.pixels.data(), dds.pixels.size());
        }
        if (!texture.image)
            return false;

        ixrhi::IXRHISamplerDesc samplerDesc;
        samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
        samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
        samplerDesc.mipmapFilter = texture.mipLevels > 1 ? ixrhi::IXRHISamplerFilter::Linear
                                                          : ixrhi::IXRHISamplerFilter::Nearest;
        samplerDesc.addressU = ixrhi::IXRHISamplerAddress::Repeat;
        samplerDesc.addressV = ixrhi::IXRHISamplerAddress::Repeat;
        samplerDesc.addressW = ixrhi::IXRHISamplerAddress::Repeat;
        samplerDesc.maxLod = static_cast<float>(texture.mipLevels);
        if (caps.supportsAnisotropy)
            samplerDesc.maxAnisotropy = caps.maxAnisotropy;
        samplerDesc.debugName = "SkinnedMesh:" + dds.filename + ":Sampler";
        texture.sampler = rhi.CreateSampler(samplerDesc);
        if (!texture.sampler)
            return false;

        LogFormat("[TEX] uploaded %s as %s (%ux%u mips=%u sampler=linear/repeat)",
            texture.name.c_str(),
            RhiFormatName(texture.format),
            texture.width,
            texture.height,
            texture.mipLevels);
    }
    m_decodedTextures.reset();
    return true;
}

bool SkinnedMeshRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    const ixrhi::IXRHIShaderStage allStages =
        ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment;
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, allStages},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
        {2, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},  // sun shadow cascades
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_uniformPages.clear();
    m_loggedUniformPagesFull = false;
    if (!AddUniformPage(rhi))
        return false;
    m_boundSunShadowTexture = m_sunShadow.sampler ? m_sunShadow.texture.get() : nullptr;

    return true;
}

SkinnedMeshRenderer::UniformPage* SkinnedMeshRenderer::AddUniformPage(ixrhi::IXRHIDevice& rhi)
{
    if (!m_bindLayout || m_uniformPages.size() >= kMaxUniformPages)
        return nullptr;
    constexpr uint32_t kIndices = kFramesInFlight * kUniformSlots;
    auto page = std::make_unique<UniformPage>();
    page->group = rhi.CreateBindGroup(*m_bindLayout, kIndices * kTextureCount);
    page->uniforms = CreateRhiBuffer(rhi,
        kUniformStride * kIndices,
        ixrhi::IXRHIBufferUsage::Uniform,
        ixrhi::IXRHICpuAccess::Write,
        nullptr,
        "SkinnedMesh:UBO");
    if (!page->group || !page->uniforms)
        return nullptr;
    for (uint32_t index = 0; index < kIndices; ++index)
    {
        for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
        {
            const uint32_t set = index * kTextureCount + textureIndex;
            page->group->UpdateBuffer(set, 0, page->uniforms, kUniformStride * index, sizeof(UniformBlock));
            const Texture& texture = m_textures[textureIndex];
            if (texture.image && texture.sampler)
                page->group->UpdateTexture(set, 1, texture.image, texture.sampler);
            if (m_sunShadow.texture && m_sunShadow.sampler)
                page->group->UpdateTexture(set, 2, m_sunShadow.texture, m_sunShadow.sampler);
        }
    }
    m_uniformPages.push_back(std::move(page));
    return m_uniformPages.back().get();
}

void SkinnedMeshRenderer::BeginFrameSlots(const ixrhi::IXRHIFrameInfo& frame)
{
    if (m_worldRenderFrameNumber == frame.frameNumber)
        return;
    m_worldRenderFrameNumber = frame.frameNumber;
    m_worldUniformCursor = 0;
}

std::optional<SkinnedMeshRenderer::UniformSlot> SkinnedMeshRenderer::NextUniformSlot(uint32_t frameIndex)
{
    if (!m_rhi || frameIndex >= kFramesInFlight)
        return std::nullopt;
    const uint32_t pageIndex = m_worldUniformCursor / kUniformSlots;
    while (pageIndex >= m_uniformPages.size())
    {
        if (!AddUniformPage(*m_rhi))
        {
            if (!m_loggedUniformPagesFull)
            {
                LogFormat("[MESH] skinned mesh draws skipped: more than %u draws of one model in a frame",
                    kMaxUniformPages * kUniformSlots);
                m_loggedUniformPagesFull = true;
            }
            return std::nullopt;
        }
    }
    UniformSlot slot;
    slot.page = m_uniformPages[pageIndex].get();
    slot.index = frameIndex * kUniformSlots + m_worldUniformCursor % kUniformSlots;
    ++m_worldUniformCursor;
    return slot;
}

void SkinnedMeshRenderer::SetSunShadow(const SunShadowReceive& shadow)
{
    m_sunShadow = shadow;
    if (m_uniformPages.empty() || !shadow.texture || !shadow.sampler || shadow.texture.get() == m_boundSunShadowTexture)
        return;
    // A new map (first one, or recreated): every set's binding 2. The map is set up front and
    // changes only with the device's resources, never while a frame uses the sets.
    for (const std::unique_ptr<UniformPage>& page : m_uniformPages)
    {
        for (uint32_t set = 0; set < kFramesInFlight * kUniformSlots * kTextureCount; ++set)
            page->group->UpdateTexture(set, 2, shadow.texture, shadow.sampler);
    }
    m_boundSunShadowTexture = shadow.texture.get();
}

bool SkinnedMeshRenderer::SunShadowBound() const
{
    if (m_boundSunShadowTexture)
        return true;
    static bool loggedNoShadowMap = false;
    if (!loggedNoShadowMap)
    {
        Log("[MESH] skinned mesh draws skipped: no sun shadow map set (SetSunShadow)");
        loggedNoShadowMap = true;
    }
    return false;
}

bool SkinnedMeshRenderer::CreateShadowPipeline(ixrhi::IXRHIDevice& rhi, const ixrhi::IXRHIRenderPass* shadowPass)
{
    if (!m_assets || !shadowPass)
        return false;
    auto vs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_shadow_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex,
        "ShadowVSMain");
    if (!vs)
        return false;

    // Depth only from the skinned vertices; the cascade's mvp comes as a push constant (no
    // descriptors). No culling, slope-scaled bias: as the terrain's and the static meshes' casters.
    ixrhi::IXRHIGraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.fragmentShader = nullptr;
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Vertex, 0, sizeof(Mat4)}};
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
    desc.debugName = "SkinnedMesh:Shadow";
    m_shadowPipeline = rhi.CreateGraphicsPipeline(desc);
    return m_shadowPipeline != nullptr;
}

void SkinnedMeshRenderer::RenderShadowCaster(ixrhi::IXRHICommandList& cmd,
    const ixrhi::IXRHIFrameInfo& frame,
    const WorldMat4& lightViewProj,
    const ixrhi::IXRHIRenderPass* shadowPass,
    WorldVec3 position,
    float yawRadians,
    uint32_t skinSlot,
    std::array<float, 3> scale)
{
    if (!m_rhi || !m_indexBuffer || m_indexCount == 0 || !shadowPass || !frame.frameActive)
        return;
    if (m_shadowPass != shadowPass)
    {
        m_shadowPipeline.reset();
        m_shadowPipelineFailed = false;
        m_shadowPass = shadowPass;
    }
    if (!m_shadowPipeline && !m_shadowPipelineFailed && !CreateShadowPipeline(*m_rhi, shadowPass))
    {
        m_shadowPipelineFailed = true;
        Log("[MESH] skinned mesh sun shadow pipeline could not be created");
    }
    if (!m_shadowPipeline)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    ixrhi::IXRHIBuffer* skinned = SkinnedOutput(frameIndex, skinSlot);
    if (!skinned)
        return;

    // The same placement as the lit draws (UpdateWorldUniform), into the cascade's light space.
    const Mat4 model = Multiply(Multiply(xm::Scale({scale[0], scale[1], scale[2]}), RotationY(-yawRadians)),
        Translation(position.x, position.y, position.z));
    const Mat4 mvp = Multiply(model, ToLocalMat4(lightViewProj));
    cmd.SetGraphicsPipeline(*m_shadowPipeline);
    cmd.PushConstants(&mvp, sizeof(mvp));
    cmd.SetVertexBuffer(0, *skinned, 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);
    for (const MeshDraw& draw : m_draws)
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
}

bool SkinnedMeshRenderer::CreateComputePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_computeBindLayout)
        return false;

    auto cs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_cs.spv",
        ixrhi::IXRHIShaderStage::Compute,
        "CSMain");
    if (!cs)
        return false;

    ixrhi::IXRHIComputePipelineDesc desc;
    desc.computeShader = cs;
    desc.bindGroupLayouts = {m_computeBindLayout.get()};
    desc.pushRanges = {{ixrhi::IXRHIShaderStage::Compute, 0, sizeof(SkinPushConstants)}};
    desc.debugName = "SkinnedMesh:Skinning";
    m_computePipeline = rhi.CreateComputePipeline(desc);
    if (!m_computePipeline)
        return false;

    LogFormat("[COMPUTE] pipeline OK, pipeline=%s", m_computePipeline->DebugName().c_str());
    return true;
}

bool SkinnedMeshRenderer::CreatePipeline(ixrhi::IXRHIDevice& rhi)
{
    if (!m_assets || !m_bindLayout)
        return false;

    auto vs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex,
        "VSMain");
    auto ps = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment,
        "PSMain");
    if (!vs || !ps)
        return false;

    // Parity with the pre-migration native graphics pipeline state: triangle
    // list, fill, cull-none, clockwise front, depth test+write LESS,
    // single opaque blend attachment, 1 sample, dynamic viewport/scissor
    // (dynamic state is implicit in the IXRHI backend).
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
    desc.debugName = "SkinnedMesh:Opaque";
    m_pipeline = rhi.CreateGraphicsPipeline(desc);
    if (!m_pipeline)
        return false;

    LogFormat("[MESH] skinned graphics pipeline OK, pipeline=%s", m_pipeline->DebugName().c_str());
    return true;
}

bool SkinnedMeshRenderer::CreateReflectionPipeline(ixrhi::IXRHIDevice& rhi,
    const ixrhi::IXRHIRenderPass* renderPass)
{
    if (!m_assets || !m_bindLayout)
        return false;

    auto vs = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_vs.spv",
        ixrhi::IXRHIShaderStage::Vertex,
        "VSMain");
    auto ps = LoadShader(rhi,
        *m_assets,
        "assets/shaders/skinned_mesh_ps.spv",
        ixrhi::IXRHIShaderStage::Fragment,
        "PSMain");
    if (!vs || !ps)
        return false;

    // Same state as the main pipeline (no culling): the reflection draws the mesh mirrored across
    // the water plane, which flips its winding.
    // A null pass means the backend default (swapchain pass); the token is
    // stored EFFECTIVE (null resolved to m_targetPass at bake time) so later
    // target swaps are detected by RenderInWorldReflection.
    const ixrhi::IXRHIRenderPass* effectivePass = renderPass != nullptr ? renderPass : m_targetPass;
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
    desc.targetRenderPass = effectivePass;
    desc.debugName = "SkinnedMesh:Reflection";
    m_reflectionPipeline = rhi.CreateGraphicsPipeline(desc);
    if (!m_reflectionPipeline)
        return false;
    m_reflectionPass = effectivePass;

    Log("[WATER-3] Skinned mesh reflection pipeline created");
    return true;
}

void SkinnedMeshRenderer::DestroyPipeline()
{
    DestroyReflectionPipeline();
    m_pipeline.reset();
    m_shadowPipeline.reset();
    m_shadowPass = nullptr;
    m_shadowPipelineFailed = false;
}

void SkinnedMeshRenderer::DestroyReflectionPipeline()
{
    m_reflectionPipeline.reset();
    m_reflectionPass = nullptr;
}

void SkinnedMeshRenderer::DestroyComputeResources()
{
    m_computePipeline.reset();
    m_skinPages.clear();
    m_computeBindLayout.reset();

    m_restVertexBuffer.reset();
}

void SkinnedMeshRenderer::DestroyAnimation()
{
    m_ozz.reset();
    m_inverseBindMatrices.clear();
    m_bonePaletteCpu.clear();
    m_importedDiffuseTexturePath.clear();
    m_boneCount = 0;
    m_motionState = MotionState::Idle;
    m_lastAnimationLogTime = -1000.0;
}

void SkinnedMeshRenderer::UpdateUniform(const UniformSlot& slot, double timeSeconds, float aspect)
{
    static bool loggedMvp = false;

    const Mat4 center = Translation(-m_bounds.center[0], -m_bounds.center[1], -m_bounds.center[2]);
    const Mat4 display = Identity();
    const Mat4 fit = Scale(m_bounds.fitScale);
    const Mat4 spin = RotationY(static_cast<float>(timeSeconds) * 0.55f);
    const Mat4 place = Translation(0.0f, 0.0f, 4.0f);
    const Mat4 model = Multiply(Multiply(Multiply(Multiply(center, display), fit), spin), place);
    const Mat4 projection = Perspective(xm::DegreesToRadians(45.0f), aspect, 0.1f, 50.0f);
    const Mat4 mvp = Multiply(model, projection);

    if (!loggedMvp)
    {
        LogFormat("[MESH] MVP: ozz/glTF mesh-local render transform, fitScale=%.3f, translateZ=4.0, fov=45, near=0.1 far=50, aspect=%.3f",
            m_bounds.fitScale,
            aspect);
        loggedMvp = true;
    }

    UniformBlock uniform{mvp, model, {1.0f, 1.0f, 1.0f, 1.0f}};
    FillLightingUniform(m_lightingState, uniform);
    WaterConfig noWater{};
    noWater.enabled = false;
    noWater.foamEnabled = false;
    noWater.causticMode = WaterConfig::CausticMode::Off;
    FillWaterUniform(noWater, timeSeconds, uniform);
    SunShadowReceive noShadow = m_sunShadow;  // the preview stands outside the world: lit
    noShadow.enabled = false;
    FillSunShadowUniform(noShadow, uniform);

    if (slot.page && slot.page->uniforms)
        slot.page->uniforms->Write(kUniformStride * slot.index, &uniform, sizeof(uniform));
}

void SkinnedMeshRenderer::UpdateWorldUniform(const UniformSlot& slot,
    const WorldCamera& camera,
    WorldVec3 position,
    float yawRadians,
    double timeSeconds,
    std::array<float, 4> tint,
    std::array<float, 3> scale,
    bool reflectionPass,
    float waterLevelY)
{
    static bool loggedMvp = false;

    // Entity scale first (in mesh-local space, where the skinned vertices live), then yaw, then
    // the world position — so a model authored in centimetres can be sized down per entity.
    const Mat4 model = Multiply(Multiply(xm::Scale({scale[0], scale[1], scale[2]}), RotationY(-yawRadians)),
        Translation(position.x, position.y, position.z));
    const Mat4 viewProjection = ToLocalMat4(camera.viewProjection);
    const Mat4 mvp = Multiply(model, viewProjection);

    if (!loggedMvp)
    {
        Log("[MESH] World MVP: ozz/glTF mesh-local render transform, spawn translated to local origin, shared full-screen camera");
        loggedMvp = true;
    }

    UniformBlock uniform{mvp, model, {tint[0], tint[1], tint[2], tint[3]}};
    if (reflectionPass)
    {
        LightingState reflectionLighting = m_lightingState;
        reflectionLighting.numPointLights = 0;
        reflectionLighting.numSpotLights = 0;
        FillLightingUniform(reflectionLighting, uniform);
        WaterConfig noWater{};
        noWater.enabled = false;
        noWater.foamEnabled = false;
        noWater.causticMode = WaterConfig::CausticMode::Off;
        FillWaterUniform(noWater, timeSeconds, uniform);
        uniform.lightPadding[0] = 1.0f;
        uniform.lightPadding[1] = waterLevelY;
    }
    else
    {
        FillLightingUniform(m_lightingState, uniform);
        WaterConfig noWater{};
        noWater.enabled = false;
        noWater.foamEnabled = false;
        noWater.causticMode = WaterConfig::CausticMode::Off;
        FillWaterUniform(noWater, timeSeconds, uniform);
    }
    FillSunShadowUniform(m_sunShadow, uniform);

    if (slot.page && slot.page->uniforms)
        slot.page->uniforms->Write(kUniformStride * slot.index, &uniform, sizeof(uniform));
}
