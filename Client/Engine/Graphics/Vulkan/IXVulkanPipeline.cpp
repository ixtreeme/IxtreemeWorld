// IXVulkan pipeline objects + device factory calls (graphics + compute).

#include "IXVulkanPipeline.h"

#include "IXVulkanBinding.h"
#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanResources.h"

namespace ixvulkan
{
namespace
{

VkPipelineLayout MakePipelineLayout(IXVulkanDevice& device,
                                    const std::vector<const ixrhi::IXRHIBindGroupLayout*>& layouts,
                                    const std::vector<ixrhi::IXRHIPushRange>& pushRanges,
                                    const char* debugName)
{
    std::vector<VkDescriptorSetLayout> nativeLayouts;
    nativeLayouts.reserve(layouts.size());
    for (const ixrhi::IXRHIBindGroupLayout* layout : layouts)
    {
        auto* native = dynamic_cast<const IXVulkanBindGroupLayout*>(layout);
        nativeLayouts.push_back(native != nullptr ? native->Native() : VK_NULL_HANDLE);
    }
    std::vector<VkPushConstantRange> nativeRanges;
    nativeRanges.reserve(pushRanges.size());
    for (const ixrhi::IXRHIPushRange& range : pushRanges)
    {
        VkPushConstantRange entry{};
        entry.stageFlags = ToVkShaderStages(range.stages);
        entry.offset = range.offsetBytes;
        entry.size = range.sizeBytes;
        nativeRanges.push_back(entry);
    }
    VkPipelineLayoutCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    create.setLayoutCount = static_cast<std::uint32_t>(nativeLayouts.size());
    create.pSetLayouts = nativeLayouts.data();
    create.pushConstantRangeCount = static_cast<std::uint32_t>(nativeRanges.size());
    create.pPushConstantRanges = nativeRanges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    IXVULKAN_CHECK(device,
        vkCreatePipelineLayout(device.NativeDevice(), &create, nullptr, &layout));
    device.SetDebugName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
        reinterpret_cast<std::uint64_t>(layout),
        debugName);
    return layout;
}

} // namespace

IXVulkanGraphicsPipeline::IXVulkanGraphicsPipeline(IXVulkanDevice& device,
                                                   VkPipeline pipeline,
                                                   VkPipelineLayout layout,
                                                   VkShaderStageFlags pushStages,
                                                   std::string debugName)
    : m_device(&device)
    , m_pipeline(pipeline)
    , m_layout(layout)
    , m_pushStages(pushStages)
    , m_debugName(std::move(debugName))
{
}

IXVulkanGraphicsPipeline::~IXVulkanGraphicsPipeline()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(native, m_pipeline, nullptr);
    if (m_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(native, m_layout, nullptr);
}

IXVulkanComputePipeline::IXVulkanComputePipeline(IXVulkanDevice& device,
                                                 VkPipeline pipeline,
                                                 VkPipelineLayout layout,
                                                 VkShaderStageFlags pushStages,
                                                 std::string debugName)
    : m_device(&device)
    , m_pipeline(pipeline)
    , m_layout(layout)
    , m_pushStages(pushStages)
    , m_debugName(std::move(debugName))
{
}

IXVulkanComputePipeline::~IXVulkanComputePipeline()
{
    if (m_device == nullptr)
        return;
    const VkDevice native = m_device->NativeDevice();
    if (m_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(native, m_pipeline, nullptr);
    if (m_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(native, m_layout, nullptr);
}

std::unique_ptr<ixrhi::IXRHIGraphicsPipeline> IXVulkanDevice::CreateGraphicsPipeline(
    const ixrhi::IXRHIGraphicsPipelineDesc& desc)
{
    if (!desc.vertexShader || !desc.fragmentShader)
        return nullptr;
    auto* vs = dynamic_cast<IXVulkanShader*>(desc.vertexShader.get());
    auto* ps = dynamic_cast<IXVulkanShader*>(desc.fragmentShader.get());
    if (vs == nullptr || ps == nullptr)
        return nullptr;

    const VkRenderPass renderPass = ResolveRenderPass(desc.targetRenderPass);
    if (renderPass == VK_NULL_HANDLE)
        return nullptr; // swapchain torn down: caller defers (parity with old code)

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->Native();
    stages[0].pName = vs->EntryPoint().c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps->Native();
    stages[1].pName = ps->EntryPoint().c_str();

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(desc.vertexBindings.size());
    for (const auto& binding : desc.vertexBindings)
    {
        VkVertexInputBindingDescription entry{};
        entry.binding = binding.binding;
        entry.stride = binding.strideBytes;
        entry.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(entry);
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(desc.vertexAttributes.size());
    for (const auto& attribute : desc.vertexAttributes)
    {
        VkVertexInputAttributeDescription entry{};
        entry.location = attribute.location;
        entry.binding = attribute.binding;
        entry.format = ToVkFormat(attribute.format);
        entry.offset = attribute.offsetBytes;
        attributes.push_back(entry);
    }
    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex.pVertexBindingDescriptions = bindings.data();
    vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = ToVkTopology(desc.topology);

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = ToVkCullMode(desc.cullMode);
    raster.frontFace = ToVkFrontFace(desc.frontFace);
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = ToVkSampleCount(desc.sampleCount);

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = ToVkCompareOp(desc.depthCompareOp);

    std::vector<VkPipelineColorBlendAttachmentState> attachments;
    attachments.reserve(desc.blendAttachments.size());
    for (const auto& blend : desc.blendAttachments)
    {
        VkPipelineColorBlendAttachmentState entry{};
        entry.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        entry.blendEnable = blend.blendEnable ? VK_TRUE : VK_FALSE;
        entry.srcColorBlendFactor = ToVkBlendFactor(blend.srcColor);
        entry.dstColorBlendFactor = ToVkBlendFactor(blend.dstColor);
        entry.colorBlendOp = ToVkBlendOp(blend.colorOp);
        entry.srcAlphaBlendFactor = ToVkBlendFactor(blend.srcAlpha);
        entry.dstAlphaBlendFactor = ToVkBlendFactor(blend.dstAlpha);
        entry.alphaBlendOp = ToVkBlendOp(blend.alphaOp);
        attachments.push_back(entry);
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    const VkPipelineLayout layout =
        MakePipelineLayout(*this, desc.bindGroupLayouts, desc.pushRanges, desc.debugName.c_str());

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &msaa;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout;
    pipeline.renderPass = renderPass;
    pipeline.subpass = 0;

    VkPipeline native = VK_NULL_HANDLE;
    CheckVk(vkCreateGraphicsPipelines(NativeDevice(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &native),
        "vkCreateGraphicsPipelines",
        __FILE__,
        __LINE__);
    SetDebugName(VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<std::uint64_t>(native),
        desc.debugName.c_str());
    VkShaderStageFlags pushStages = 0;
    for (const ixrhi::IXRHIPushRange& range : desc.pushRanges)
        pushStages |= ToVkShaderStages(range.stages);
    return std::make_unique<IXVulkanGraphicsPipeline>(*this, native, layout, pushStages, desc.debugName);
}

std::unique_ptr<ixrhi::IXRHIComputePipeline> IXVulkanDevice::CreateComputePipeline(
    const ixrhi::IXRHIComputePipelineDesc& desc)
{
    if (!desc.computeShader)
        return nullptr;
    auto* cs = dynamic_cast<IXVulkanShader*>(desc.computeShader.get());
    if (cs == nullptr)
        return nullptr;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs->Native();
    stage.pName = cs->EntryPoint().c_str();

    const VkPipelineLayout layout =
        MakePipelineLayout(*this, desc.bindGroupLayouts, desc.pushRanges, desc.debugName.c_str());

    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage = stage;
    pipeline.layout = layout;
    VkPipeline native = VK_NULL_HANDLE;
    CheckVk(vkCreateComputePipelines(NativeDevice(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &native),
        "vkCreateComputePipelines",
        __FILE__,
        __LINE__);
    SetDebugName(VK_OBJECT_TYPE_PIPELINE,
        reinterpret_cast<std::uint64_t>(native),
        desc.debugName.c_str());
    VkShaderStageFlags pushStages = 0;
    for (const ixrhi::IXRHIPushRange& range : desc.pushRanges)
        pushStages |= ToVkShaderStages(range.stages);
    return std::make_unique<IXVulkanComputePipeline>(*this, native, layout, pushStages, desc.debugName);
}

} // namespace ixvulkan
