#include "StaticMeshRenderer.h"

#include "Debug.h"
#include "asset/IAssetReader.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
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
    fastgltf::Parser parser;
    auto assetResult = parser.loadGltf(data.get(), std::filesystem::path(dir),
        fastgltf::Options::DecomposeNodeMatrices);
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

bool LoadGltfBaseColorTexture(client::asset::IAssetReader& assets,
    const std::string& modelPath,
    RgbaImage& out)
{
    std::string error;
    auto parsed = ParseGltf(assets, modelPath, &error);
    if (!parsed)
        return false;
    const fastgltf::Asset& asset = *parsed;
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

        out.name = std::string(image.name.empty() ? "glTF baseColorTexture" : image.name);
        out.width = static_cast<uint32_t>(width);
        out.height = static_cast<uint32_t>(height);
        out.format = VK_FORMAT_R8G8B8A8_SRGB;
        out.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        stbi_image_free(decoded);
        LogFormat("[STATIC-MESH] decoded baseColorTexture material='%s' image='%s' (%ux%u, source channels=%d)",
            material.name.c_str(), out.name.c_str(), out.width, out.height, channels);
        return true;
    }
    return false;
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
    m_status = LoadStatus::Failed;

    bool isSkinned = false;
    std::string error;
    if (!DetectSkinnedGltf(assets, modelPath, isSkinned, &error))
    {
        LogFormat("[STATIC-MESH] inspect failed: %s reason=%s", modelPath.c_str(), error.c_str());
        return false;
    }
    if (isSkinned)
    {
        m_status = LoadStatus::UnsupportedSkinned;
        LogFormat("[STATIC-MESH] skinned glTF detected, static renderer will not load it: %s", modelPath.c_str());
        return false;
    }

    if (!LoadStaticGltfMesh(modelPath))
        return false;
    if (!CreateBuffers(device))
        return false;
    if (!CreateTexture(device, modelPath))
        return false;
    if (!CreateDescriptors())
        return false;
    if (!CreatePipeline(device))
        return false;

    m_status = LoadStatus::LoadedStatic;
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
            m_draws.push_back(draw);
            LogFormat("[STATIC-MESH] extracted primitive[%u] mesh='%s': verts=%zu indices=%u",
                primitiveIndex++, mesh.name.c_str(), vertices.size(), draw.indexCount);
        }
    }

    if (m_vertices.empty() || m_indices.empty())
    {
        LogFormat("[STATIC-MESH] no renderable static mesh data extracted: %s", modelPath.c_str());
        return false;
    }
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
    return true;
}

bool StaticMeshRenderer::CreateTexture(VulkanDevice& device, const std::string& modelPath)
{
    RgbaImage image{};
    if (!m_assets || !LoadGltfBaseColorTexture(*m_assets, modelPath, image))
    {
        image = CreateFallbackWhiteImage(modelPath);
        LogFormat("[STATIC-MESH] using fallback white texture for %s", modelPath.c_str());
    }

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), image.format, &props);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((props.optimalTilingFeatures & required) != required)
    {
        image = CreateFallbackWhiteImage(modelPath);
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), image.format, &props);
        if ((props.optimalTilingFeatures & required) != required)
            return false;
    }

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, device.GetGraphicsQueueFamily(), 0, &graphicsQueue);

    m_texture.name = image.name;
    m_texture.width = image.width;
    m_texture.height = image.height;
    m_texture.mipLevels = 1;
    m_texture.format = image.format;
    CreateDeviceLocalImage(device, m_device, image.width, image.height, image.format, m_texture.image, m_texture.memory);

    Buffer staging{};
    CreateHostVisibleBuffer(device, m_device, image.pixels.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, image.pixels.data(), staging);

    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = BeginOneTimeCommands(m_device, device.GetGraphicsQueueFamily(), uploadPool);
    TransitionImageLayout(cmd, m_texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_texture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImageLayout(cmd, m_texture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneTimeCommands(m_device, graphicsQueue, uploadPool, cmd);
    DestroyBuffer(staging);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = m_texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = m_texture.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.baseMipLevel = 0;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.baseArrayLayer = 0;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &view, nullptr, &m_texture.view));

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
    VK_CHECK(vkCreateSampler(m_device, &sampler, nullptr, &m_texture.sampler));
    return true;
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

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {ubo, diffuse};
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight * kUniformSlots;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight * kUniformSlots;

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

            std::array<VkWriteDescriptorSet, 2> writes{};
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
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
    return true;
}

bool StaticMeshRenderer::CreatePipeline(VulkanDevice& device)
{
    if (!m_assets)
        return false;

    VkShaderModule vs = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, *m_assets, "assets/shaders/skinned_mesh_ps.spv");

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
    if (!m_pipeline || m_indices.empty() || !device.IsFrameActive())
        return;
    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const uint32_t frameIndex = device.GetFrameIndex();
    if (m_worldRenderFrameIndex != frameIndex)
    {
        m_worldRenderFrameIndex = frameIndex;
        m_worldUniformCursor = 0;
    }
    const uint32_t uniformSlot = std::min(m_worldUniformCursor++, kUniformSlots - 1);
    UpdateWorldUniform(frameIndex, uniformSlot, camera, instance, timeSeconds);

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
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
        0, 1, &m_descriptorSets[frameIndex][uniformSlot], 0, nullptr);
    for (const MeshDraw& draw : m_draws)
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, 0, 0);
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
    for (auto& frameBuffers : m_uniformBuffers)
    {
        for (Buffer& buffer : frameBuffers)
            DestroyBuffer(buffer);
    }
    DestroyTexture(m_texture);
    m_vertices.clear();
    m_indices.clear();
    m_draws.clear();
    m_assets = nullptr;
    m_status = LoadStatus::NotLoaded;
    m_device = VK_NULL_HANDLE;
}

void StaticMeshRenderer::UpdateWorldUniform(uint32_t frameIndex,
    uint32_t uniformSlot,
    const WorldCamera& camera,
    const Instance& instance,
    double)
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
    FillLightingUniform(m_lightingState, uniform);
    uniform.waterParams[0] = 0.0f;
    uniform.causticParams[0] = 0.0f;

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex][uniformSlot].memory);
}
