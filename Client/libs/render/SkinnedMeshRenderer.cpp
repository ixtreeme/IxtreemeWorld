#include "SkinnedMeshRenderer.h"

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
};

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
    m_rhi = &rhi;
    m_assets = &assets;

    std::string ext = std::filesystem::path(modelPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool loaded = ext == ".fbx" ? LoadFbxMesh(modelPath) : LoadGltfMesh(modelPath);
    const bool buffers = loaded ? CreateBuffers(rhi) : false;
    const bool compute = buffers ? CreateComputeResources(rhi) : false;
    const bool textures = compute ? CreateTextures(rhi, modelPath) : false;
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
    if (skinSlot >= kSkinSlots)
        return;

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
    if (skinSlot >= kSkinSlots)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (frameIndex >= kFramesInFlight || !m_bonePaletteBuffers[frameIndex][skinSlot] ||
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

    if (!m_pipeline || !m_bindGroup || !m_indexBuffer || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] Render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }

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
    if (!m_skinnedOutputBuffers[frameIndex][0])
        return;
    const float aspect = static_cast<float>(rect.width) / static_cast<float>(rect.height);
    UpdateUniform(frameIndex, 0, timeSeconds, aspect);

    const std::uint32_t clearX = static_cast<std::uint32_t>(std::max<std::int32_t>(rect.x, 0));
    const std::uint32_t clearY = static_cast<std::uint32_t>(std::max<std::int32_t>(rect.y, 0));
    cmd.ClearDepth(1.0f, clearX, clearY, rect.width, rect.height);

    cmd.SetViewport(static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height));
    cmd.SetScissor(clearX, clearY, rect.width, rect.height);
    cmd.SetGraphicsPipeline(*m_pipeline);

    cmd.SetVertexBuffer(0, *m_skinnedOutputBuffers[frameIndex][0], 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    static bool loggedDraws = false;
    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        const MeshDraw& draw = m_draws[i];
        const std::uint32_t slot =
            (frameIndex * kUniformSlots + 0u) * kTextureCount + draw.textureIndex;
        cmd.BindGroup(0, *m_bindGroup, slot);
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
    std::uint32_t targetHeight)
{
    static bool loggedDraw = false;
    static bool loggedNoPipeline = false;

    if (!m_pipeline || !m_bindGroup || !m_indexBuffer || m_indexCount == 0)
    {
        if (!loggedNoPipeline)
        {
            Log("[MESH] World render skip: no pipeline or indices");
            loggedNoPipeline = true;
        }
        return;
    }

    if (!frame.frameActive || frame.commandList == nullptr)
        return;

    const std::uint32_t extentWidth = targetWidth > 0 ? targetWidth : frame.targetWidth;
    const std::uint32_t extentHeight = targetHeight > 0 ? targetHeight : frame.targetHeight;
    if (extentWidth == 0 || extentHeight == 0)
        return;

    const uint32_t frameIndex = frame.frameIndex % kFramesInFlight;
    if (skinSlot >= kSkinSlots)
        skinSlot = 0;
    if (!m_skinnedOutputBuffers[frameIndex][skinSlot])
        return;
    if (m_worldRenderFrameIndex != frameIndex)
    {
        m_worldRenderFrameIndex = frameIndex;
        m_worldUniformCursor = 0;
    }

    const uint32_t uniformSlot = std::min(m_worldUniformCursor++, kUniformSlots - 1);
    UpdateWorldUniform(frameIndex, uniformSlot, camera, position, yawRadians, timeSeconds, tint);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(extentWidth), static_cast<float>(extentHeight));
    cmd.SetScissor(0, 0, extentWidth, extentHeight);
    cmd.SetGraphicsPipeline(*m_pipeline);

    cmd.SetVertexBuffer(0, *m_skinnedOutputBuffers[frameIndex][skinSlot], 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    for (const MeshDraw& draw : m_draws)
    {
        const std::uint32_t slot =
            (frameIndex * kUniformSlots + uniformSlot) * kTextureCount + draw.textureIndex;
        cmd.BindGroup(0, *m_bindGroup, slot);
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
    std::array<float, 4> tint)
{
    // Reflection draws run inside the terrain-owned native reflection pass.
    // That pass uses the swapchain color/depth formats (same as the backend
    // main pass and the offscreen scene pass), so a pipeline baked against
    // the borrowed IXRHI pass token is attachment-compatible. The native
    // render-pass handle never crosses into this renderer.
    if (!m_bindLayout || !m_pipeline || !m_bindGroup || !m_indexBuffer ||
        m_indexCount == 0 || !frame.frameActive || frame.commandList == nullptr || m_rhi == nullptr)
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
    if (skinSlot >= kSkinSlots)
        skinSlot = 0;
    if (!m_skinnedOutputBuffers[frameIndex][skinSlot])
        return;
    if (m_worldRenderFrameIndex != frameIndex)
    {
        m_worldRenderFrameIndex = frameIndex;
        m_worldUniformCursor = 0;
    }

    const uint32_t uniformSlot = std::min(m_worldUniformCursor++, kUniformSlots - 1);
    UpdateWorldUniform(frameIndex, uniformSlot, camera, position, yawRadians, 0.0, tint, true, waterLevelY);

    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight));
    cmd.SetScissor(0, 0, targetWidth, targetHeight);
    cmd.SetGraphicsPipeline(*m_reflectionPipeline);

    cmd.SetVertexBuffer(0, *m_skinnedOutputBuffers[frameIndex][skinSlot], 0);
    cmd.SetIndexBuffer(*m_indexBuffer, 0, /*thirtyTwoBit=*/true);

    for (const MeshDraw& draw : m_draws)
    {
        const std::uint32_t slot =
            (frameIndex * kUniformSlots + uniformSlot) * kTextureCount + draw.textureIndex;
        cmd.BindGroup(0, *m_bindGroup, slot);
        cmd.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void SkinnedMeshRenderer::Destroy()
{
    DestroyAnimation();
    DestroyPipeline();
    DestroyComputeResources();

    m_bindGroup.reset();
    m_bindLayout.reset();
    m_indexBuffer.reset();
    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (auto& buffer : frameBuffers)
            buffer.reset();
    }
    for (Texture& texture : m_textures)
        texture = {};
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
    if (!std::filesystem::exists(stemSkeleton))
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
        samplingJob.context = &m_ozz->context;
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
    if (!m_ozz || m_boneCount == 0)
        return false;

    ozz::animation::LocalToModelJob localToModel;
    localToModel.skeleton = &m_ozz->skeleton;
    localToModel.input = locals;
    localToModel.output = ozz::make_span(m_ozz->models);
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
        m_bonePaletteCpu[bone] = ToRowVectorPaletteMatrix(m_ozz->models[bone] * inverseBind);
    }
    return true;
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
    if (frameIndex >= kFramesInFlight || skinSlot >= kSkinSlots ||
        !m_bonePaletteBuffers[frameIndex][skinSlot] || m_bonePaletteCpu.empty())
    {
        return false;
    }

    const std::size_t size = sizeof(Mat4) * m_bonePaletteCpu.size();
    m_bonePaletteBuffers[frameIndex][skinSlot]->Write(0, m_bonePaletteCpu.data(), size);
    return true;
}

bool SkinnedMeshRenderer::UploadBonePalette(float animTimeSeconds, uint32_t frameIndex)
{
    return UploadBonePalette(m_motionState, animTimeSeconds, frameIndex, 0);
}

bool SkinnedMeshRenderer::UploadBonePalette(MotionState state, float animTimeSeconds, uint32_t frameIndex, uint32_t skinSlot)
{
    if (frameIndex >= kFramesInFlight || skinSlot >= kSkinSlots || !m_bonePaletteBuffers[frameIndex][skinSlot] ||
        !m_ozz || m_bonePaletteCpu.empty())
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

void SkinnedMeshRenderer::DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex)
{
    DispatchSkin(cmd, frameIndex, 0);
}

void SkinnedMeshRenderer::DispatchSkin(ixrhi::IXRHICommandList& cmd, uint32_t frameIndex, uint32_t skinSlot)
{
    cmd.SetComputePipeline(*m_computePipeline);
    cmd.BindGroup(0, *m_computeBindGroup, frameIndex * kSkinSlots + skinSlot);

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
    cmd.TransitionBuffer(*m_skinnedOutputBuffers[frameIndex][skinSlot],
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
    rhi.ExecuteAndWait([&](ixrhi::IXRHICommandList& cmd) {
        DispatchSkin(cmd, 0);
        cmd.TransitionBuffer(*m_skinnedOutputBuffers[0][0],
            ixrhi::IXRHIBufferState::ShaderWrite,
            ixrhi::IXRHIBufferState::TransferSrc);
        cmd.CopyBuffer(*m_skinnedOutputBuffers[0][0], *staging, vertexSize);
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
    m_indexBuffer = CreateRhiBuffer(rhi,
        sizeof(uint32_t) * m_indices.size(),
        ixrhi::IXRHIBufferUsage::Index,
        ixrhi::IXRHICpuAccess::Write,
        m_indices.data(),
        "SkinnedMesh:IB");

    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (auto& buffer : frameBuffers)
        {
            buffer = CreateRhiBuffer(rhi,
                sizeof(UniformBlock),
                ixrhi::IXRHIBufferUsage::Uniform,
                ixrhi::IXRHICpuAccess::Write,
                nullptr,
                "SkinnedMesh:UBO");
            if (!buffer)
                return false;
        }
    }
    return m_indexBuffer != nullptr;
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

    m_restVertexBuffer = CreateRhiBuffer(rhi,
        restSize,
        ixrhi::IXRHIBufferUsage::Storage,
        ixrhi::IXRHICpuAccess::None,
        m_restVerticesGpu.data(),
        "SkinnedMesh:RestVertices");

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t skinSlot = 0; skinSlot < kSkinSlots; ++skinSlot)
        {
            m_bonePaletteBuffers[frame][skinSlot] = CreateRhiBuffer(rhi,
                paletteSize,
                ixrhi::IXRHIBufferUsage::Storage,
                ixrhi::IXRHICpuAccess::Write,
                nullptr,
                "SkinnedMesh:BonePalette");
            // Skinned output is written by compute and read as vertex input
            // (plus transfer-source for verification readback).
            m_skinnedOutputBuffers[frame][skinSlot] = CreateRhiBuffer(rhi,
                vertexSize,
                ixrhi::IXRHIBufferUsage::Storage | ixrhi::IXRHIBufferUsage::Vertex |
                    ixrhi::IXRHIBufferUsage::TransferSrc,
                ixrhi::IXRHICpuAccess::None,
                nullptr,
                "SkinnedMesh:SkinnedOutput");
            if (!m_bonePaletteBuffers[frame][skinSlot] || !m_skinnedOutputBuffers[frame][skinSlot])
                return false;
        }
    }
    if (!m_restVertexBuffer)
        return false;

    if (!CreateComputeBindGroup(rhi))
        return false;
    if (!CreateComputePipeline(rhi))
        return false;
    if (!VerifyComputeSkin(rhi))
        return false;

    LogFormat("[COMPUTE] resources OK rest=%zu bytes output/frame/slot=%zu bytes palette/frame/slot=%zu bytes slots=%u",
        static_cast<size_t>(restSize),
        static_cast<size_t>(vertexSize),
        static_cast<size_t>(paletteSize),
        kSkinSlots);
    return true;
}

bool SkinnedMeshRenderer::CreateTextures(ixrhi::IXRHIDevice& rhi, const std::string& modelPath)
{
    const std::array<std::string, kTextureCount> textureFiles = {
        modelPath + "#baseColor",
        modelPath + "#fallback"};

    const ixrhi::IXRHITextureUsage sampledUpload =
        ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    const ixrhi::IXRHICapabilities& caps = rhi.GetCapabilities();

    for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
    {
        DdsImage dds{};
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
        texture.image = rhi.CreateTexture(imageDesc, dds.pixels.data(), dds.pixels.size());
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

    return true;
}

bool SkinnedMeshRenderer::CreateBindGroup(ixrhi::IXRHIDevice& rhi)
{
    constexpr uint32_t kBindSlots = kFramesInFlight * kUniformSlots * kTextureCount;
    const ixrhi::IXRHIShaderStage allStages =
        ixrhi::IXRHIShaderStage::Vertex | ixrhi::IXRHIShaderStage::Fragment;
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::UniformBuffer, allStages},
        {1, ixrhi::IXRHIBindingType::SampledTexture, ixrhi::IXRHIShaderStage::Fragment},
    };
    m_bindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_bindLayout)
        return false;
    m_bindGroup = rhi.CreateBindGroup(*m_bindLayout, kBindSlots);
    if (!m_bindGroup)
        return false;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t uniformSlot = 0; uniformSlot < kUniformSlots; ++uniformSlot)
        {
            for (uint32_t textureIndex = 0; textureIndex < kTextureCount; ++textureIndex)
            {
                const uint32_t slot = (frame * kUniformSlots + uniformSlot) * kTextureCount + textureIndex;
                m_bindGroup->UpdateBuffer(slot,
                    0,
                    m_uniformBuffers[frame][uniformSlot],
                    0,
                    sizeof(UniformBlock));
                const Texture& texture = m_textures[textureIndex];
                if (texture.image && texture.sampler)
                    m_bindGroup->UpdateTexture(slot, 1, texture.image, texture.sampler);
            }
        }
    }

    return true;
}

bool SkinnedMeshRenderer::CreateComputeBindGroup(ixrhi::IXRHIDevice& rhi)
{
    constexpr uint32_t kComputeSets = kFramesInFlight * kSkinSlots;
    const std::vector<ixrhi::IXRHIBinding> bindings = {
        {0, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
        {1, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
        {2, ixrhi::IXRHIBindingType::StorageBuffer, ixrhi::IXRHIShaderStage::Compute},
    };
    m_computeBindLayout = rhi.CreateBindGroupLayout(bindings);
    if (!m_computeBindLayout)
        return false;
    m_computeBindGroup = rhi.CreateBindGroup(*m_computeBindLayout, kComputeSets);
    if (!m_computeBindGroup)
        return false;

    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        for (uint32_t skinSlot = 0; skinSlot < kSkinSlots; ++skinSlot)
        {
            const uint32_t slot = frame * kSkinSlots + skinSlot;
            m_computeBindGroup->UpdateBuffer(slot,
                0,
                m_restVertexBuffer,
                0,
                sizeof(RestVertexGpu) * m_restVerticesGpu.size());
            m_computeBindGroup->UpdateBuffer(slot,
                1,
                m_bonePaletteBuffers[frame][skinSlot],
                0,
                sizeof(Mat4) * static_cast<size_t>(m_boneCount));
            m_computeBindGroup->UpdateBuffer(slot,
                2,
                m_skinnedOutputBuffers[frame][skinSlot],
                0,
                sizeof(Vertex) * m_vertices.size());
        }
    }

    Log("[COMPUTE] descriptor sets created");
    return true;
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

    // Same state as the main pipeline except front-face culling (mirrored
    // winding seen from below the water plane) — exactly like before.
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
    desc.cullMode = ixrhi::IXRHICullMode::Front;
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
}

void SkinnedMeshRenderer::DestroyReflectionPipeline()
{
    m_reflectionPipeline.reset();
    m_reflectionPass = nullptr;
}

void SkinnedMeshRenderer::DestroyComputeResources()
{
    m_computePipeline.reset();
    m_computeBindGroup.reset();
    m_computeBindLayout.reset();

    m_restVertexBuffer.reset();
    for (auto& frameBuffers : m_bonePaletteBuffers)
    {
        for (auto& buffer : frameBuffers)
            buffer.reset();
    }
    for (auto& frameBuffers : m_skinnedOutputBuffers)
    {
        for (auto& buffer : frameBuffers)
            buffer.reset();
    }
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

void SkinnedMeshRenderer::UpdateUniform(uint32_t frameIndex, uint32_t uniformSlot, double timeSeconds, float aspect)
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

    if (frameIndex < kFramesInFlight && uniformSlot < kUniformSlots &&
        m_uniformBuffers[frameIndex][uniformSlot])
        m_uniformBuffers[frameIndex][uniformSlot]->Write(0, &uniform, sizeof(uniform));
}

void SkinnedMeshRenderer::UpdateWorldUniform(uint32_t frameIndex,
    uint32_t uniformSlot,
    const WorldCamera& camera,
    WorldVec3 position,
    float yawRadians,
    double timeSeconds,
    std::array<float, 4> tint,
    bool reflectionPass,
    float waterLevelY)
{
    static bool loggedMvp = false;

    const Mat4 model = Multiply(RotationY(-yawRadians),
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

    if (frameIndex < kFramesInFlight && uniformSlot < kUniformSlots &&
        m_uniformBuffers[frameIndex][uniformSlot])
        m_uniformBuffers[frameIndex][uniformSlot]->Write(0, &uniform, sizeof(uniform));
}
