#include "CubeRenderer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
void Log(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", text);
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

unsigned long long HandleValue(VkPipeline handle)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle));
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

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s:%d: Vulkan call failed: %s -> %s (%d)",
        file, line, call, VkResultName(result), result);
    Log(buffer);
    std::abort();
}

#define VK_CHECK(call) CheckVk((call), #call, __FILE__, __LINE__)

struct Vertex
{
    float position[3];
    float color[3];
};

struct Mat4
{
    float m[16];
};

struct UniformBlock
{
    Mat4 mvp;
};

constexpr std::array<Vertex, 8> kVertices =
{{
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.2f, 0.2f}},
    {{ 0.5f, -0.5f, -0.5f}, {0.2f, 1.0f, 0.2f}},
    {{ 0.5f,  0.5f, -0.5f}, {0.2f, 0.4f, 1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {1.0f, 0.9f, 0.2f}},
    {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.2f, 1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, {0.2f, 1.0f, 1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, {0.9f, 0.9f, 1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {1.0f, 0.6f, 0.2f}},
}};

constexpr std::array<uint16_t, 36> kIndices =
{{
    0, 1, 2, 2, 3, 0,
    1, 5, 6, 6, 2, 1,
    5, 4, 7, 7, 6, 5,
    4, 0, 3, 3, 7, 4,
    3, 2, 6, 6, 7, 3,
    4, 5, 1, 1, 0, 4,
}};

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

Mat4 Rotation(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);

    Mat4 y = Identity();
    y.m[0] = c;
    y.m[2] = s;
    y.m[8] = -s;
    y.m[10] = c;

    Mat4 x = Identity();
    x.m[5] = c;
    x.m[6] = -s;
    x.m[9] = s;
    x.m[10] = c;

    return Multiply(x, y);
}

Mat4 Scale(float value)
{
    Mat4 r = Identity();
    r.m[0] = value;
    r.m[5] = value;
    r.m[10] = value;
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

Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zFar - zNear);
    r.m[11] = 1.0f;
    r.m[14] = -(zNear * zFar) / (zFar - zNear);
    return r;
}

std::string ExecutableDirectory()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string result(path);
    const size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : result.substr(0, slash);
}

std::vector<char> ReadBinaryFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file)
    {
        std::string message = "Failed to open shader: " + path;
        Log(message.c_str());
        std::abort();
    }

    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> bytes(size);
    file.seekg(0);
    file.read(bytes.data(), bytes.size());
    return bytes;
}

VkShaderModule CreateShaderModule(VkDevice device, const std::string& path)
{
    const std::vector<char> code = ReadBinaryFile(path);
    VkShaderModuleCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &create, nullptr, &module));
    return module;
}

bool CreateHostVisibleBuffer(VulkanDevice& device, VkDevice vkDevice, VkDeviceSize size,
    VkBufferUsageFlags usage, const void* initialData, CubeRenderer::Buffer& out)
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

VkRect2D PreviewRect(VkExtent2D extent)
{
    // TODO: derive this from the actual Lobby.xaml preview-frame bounds instead of mirroring the current fixed layout.
    const int32_t cardWidth = 940;
    const int32_t cardHeight = 492;
    const int32_t cardX = std::max<int32_t>(0, (static_cast<int32_t>(extent.width) - cardWidth) / 2);
    const int32_t cardY = std::max<int32_t>(0, (static_cast<int32_t>(extent.height) - cardHeight) / 2);

    VkRect2D rect{};
    rect.offset.x = cardX + 46;
    rect.offset.y = cardY + 96;
    rect.extent.width = 224;
    rect.extent.height = 324;

    if (rect.offset.x < 0) rect.offset.x = 0;
    if (rect.offset.y < 0) rect.offset.y = 0;
    if (rect.offset.x + static_cast<int32_t>(rect.extent.width) > static_cast<int32_t>(extent.width))
        rect.extent.width = extent.width - rect.offset.x;
    if (rect.offset.y + static_cast<int32_t>(rect.extent.height) > static_cast<int32_t>(extent.height))
        rect.extent.height = extent.height - rect.offset.y;
    return rect;
}

void LogNdcZRangeOnce(const Mat4& mvp)
{
    static bool logged = false;
    if (logged)
        return;

    logged = true;
    float minZ = std::numeric_limits<float>::max();
    float maxZ = -std::numeric_limits<float>::max();

    for (const Vertex& vertex : kVertices)
    {
        const float x = vertex.position[0];
        const float y = vertex.position[1];
        const float z = vertex.position[2];
        const float clipZ = x * mvp.m[2] + y * mvp.m[6] + z * mvp.m[10] + mvp.m[14];
        const float clipW = x * mvp.m[3] + y * mvp.m[7] + z * mvp.m[11] + mvp.m[15];
        const float ndcZ = clipW != 0.0f ? clipZ / clipW : 0.0f;
        minZ = std::min(minZ, ndcZ);
        maxZ = std::max(maxZ, ndcZ);
    }

    LogFormat("[CUBE] clip-space NDC Z range: min=%.3f max=%.3f", minZ, maxZ);
}
}

bool CubeRenderer::Create(VulkanDevice& device)
{
    Destroy();
    m_device = device.GetDevice();

    const bool buffers = CreateBuffers(device);
    const bool descriptors = buffers ? CreateDescriptors(device) : false;
    const bool pipeline = descriptors ? CreatePipeline(device) : false;
    LogFormat("[CUBE] Create: buffers=%d descriptors=%d pipeline=%d",
        buffers ? 1 : 0,
        descriptors ? 1 : 0,
        pipeline ? 1 : 0);

    if (buffers && descriptors && pipeline)
        LogFormat("[CUBE] Create OK, pipeline=0x%llx", HandleValue(m_pipeline));

    return buffers && descriptors && pipeline;
}

bool CubeRenderer::RecreatePipeline(VulkanDevice& device)
{
    if (!m_device)
        return true;

    DestroyPipeline();
    if (device.GetRenderPass() == VK_NULL_HANDLE)
        return true;

    return CreatePipeline(device);
}

void CubeRenderer::Render(VulkanDevice& device, double timeSeconds)
{
    static bool loggedNoPipeline = false;
    static bool loggedFrameInactive = false;
    static bool loggedExtentZero = false;
    static bool loggedRectZero = false;
    static bool loggedDraw = false;

    if (!m_pipeline)
    {
        if (!loggedNoPipeline)
        {
            Log("[CUBE] Render skip: no pipeline");
            loggedNoPipeline = true;
        }
        return;
    }

    if (!device.IsFrameActive())
    {
        if (!loggedFrameInactive)
        {
            Log("[CUBE] Render skip: frame inactive");
            loggedFrameInactive = true;
        }
        return;
    }

    const VkExtent2D extent = device.GetSwapchainExtent();
    if (extent.width == 0 || extent.height == 0)
    {
        if (!loggedExtentZero)
        {
            Log("[CUBE] Render skip: extent 0");
            loggedExtentZero = true;
        }
        return;
    }

    const VkRect2D rect = PreviewRect(extent);
    if (rect.extent.width == 0 || rect.extent.height == 0)
    {
        if (!loggedRectZero)
        {
            LogFormat("[CUBE] Render skip: preview rect 0 (extent=%ux%u)",
                extent.width,
                extent.height);
            loggedRectZero = true;
        }
        return;
    }

    if (!loggedDraw)
    {
        LogFormat("[CUBE] Render: drawing in rect x=%d y=%d w=%u h=%u (swapchain %ux%u)",
            rect.offset.x,
            rect.offset.y,
            rect.extent.width,
            rect.extent.height,
            extent.width,
            extent.height);
        loggedDraw = true;
    }

    const uint32_t frameIndex = device.GetFrameIndex();
    const float aspect = static_cast<float>(rect.extent.width) / static_cast<float>(rect.extent.height);
    UpdateUniform(frameIndex, timeSeconds, aspect);

    VkCommandBuffer cmd = device.GetCommandBuffer();

    VkClearAttachment depthClear{};
    depthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthClear.clearValue.depthStencil.depth = 1.0f;

    VkClearRect depthRect{};
    depthRect.rect = rect;
    depthRect.baseArrayLayer = 0;
    depthRect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &depthClear, 1, &depthRect);

    VkViewport viewport{};
    viewport.x = static_cast<float>(rect.offset.x);
    viewport.y = static_cast<float>(rect.offset.y);
    viewport.width = static_cast<float>(rect.extent.width);
    viewport.height = static_cast<float>(rect.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
        0, 1, &m_descriptorSets[frameIndex], 0, nullptr);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(kIndices.size()), 1, 0, 0, 0);

    VkViewport fullViewport{};
    fullViewport.x = 0.0f;
    fullViewport.y = 0.0f;
    fullViewport.width = static_cast<float>(extent.width);
    fullViewport.height = static_cast<float>(extent.height);
    fullViewport.minDepth = 0.0f;
    fullViewport.maxDepth = 1.0f;
    VkRect2D fullScissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &fullViewport);
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);
}

void CubeRenderer::Destroy()
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

    DestroyBuffer(m_indexBuffer);
    DestroyBuffer(m_vertexBuffer);
    for (Buffer& buffer : m_uniformBuffers)
        DestroyBuffer(buffer);

    m_device = VK_NULL_HANDLE;
}

bool CubeRenderer::CreateBuffers(VulkanDevice& device)
{
    CreateHostVisibleBuffer(device, m_device, sizeof(kVertices),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kVertices.data(), m_vertexBuffer);
    CreateHostVisibleBuffer(device, m_device, sizeof(kIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, kIndices.data(), m_indexBuffer);

    for (Buffer& buffer : m_uniformBuffers)
    {
        CreateHostVisibleBuffer(device, m_device, sizeof(UniformBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, buffer);
    }
    return true;
}

bool CubeRenderer::CreateDescriptors(VulkanDevice&)
{
    VkDescriptorSetLayoutBinding ubo{};
    ubo.binding = 0;
    ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo.descriptorCount = 1;
    ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings = &ubo;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &m_descriptorSetLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool));

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(m_descriptorSetLayout);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(m_device, &alloc, m_descriptorSets.data()));

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i].buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBlock);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    return true;
}

bool CubeRenderer::CreatePipeline(VulkanDevice& device)
{
    const std::string shaderDir = ExecutableDirectory() + "\\shaders\\";
    VkShaderModule vs = CreateShaderModule(m_device, shaderDir + "cube_vs.spv");
    VkShaderModule ps = CreateShaderModule(m_device, shaderDir + "cube_ps.spv");

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

    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
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
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
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
    pipeline.renderPass = device.GetRenderPass();
    pipeline.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m_pipeline));

    vkDestroyShaderModule(m_device, ps, nullptr);
    vkDestroyShaderModule(m_device, vs, nullptr);
    return true;
}

void CubeRenderer::DestroyPipeline()
{
    if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    if (m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
}

void CubeRenderer::DestroyBuffer(Buffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

void CubeRenderer::UpdateUniform(uint32_t frameIndex, double timeSeconds, float aspect)
{
    static bool loggedMvp = false;
    if (!loggedMvp)
    {
        const float z = 1.8f;
        const float zNear = 0.1f;
        const float zFar = 20.0f;
        const float ndcZ = (zFar / (zFar - zNear)) - ((zNear * zFar) / (zFar - zNear)) / z;
        LogFormat("[CUBE] MVP: scale=1.4, rotation=time, translation=(0,0,1.8), fov=60, near=0.1 far=20, aspect=%.3f, center ndcZ=%.3f",
            aspect,
            ndcZ);
        loggedMvp = true;
    }

    const Mat4 model = Multiply(Multiply(Scale(1.4f), Rotation(static_cast<float>(timeSeconds))), Translation(0.0f, 0.0f, 1.8f));
    const Mat4 projection = Perspective(60.0f * 3.1415926535f / 180.0f, aspect, 0.1f, 20.0f);
    const Mat4 mvp = Multiply(model, projection);
    LogNdcZRangeOnce(mvp);
    const UniformBlock uniform{mvp};

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device, m_uniformBuffers[frameIndex].memory, 0, sizeof(uniform), 0, &mapped));
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(m_device, m_uniformBuffers[frameIndex].memory);
}
