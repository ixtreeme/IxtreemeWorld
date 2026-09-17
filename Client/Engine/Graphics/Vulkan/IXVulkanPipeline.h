#pragma once

// IXVulkan pipeline objects own VkPipeline + VkPipelineLayout (all variants the
// desc implies are baked at creation, same as before: no pipeline cache redesign
// in Phase 2 — see docs; creation cost unchanged).

#include "IXRHIPipeline.h"

#include <vulkan/vulkan.h>

#include <string>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanGraphicsPipeline final : public ixrhi::IXRHIGraphicsPipeline
{
public:
    IXVulkanGraphicsPipeline(IXVulkanDevice& device,
                             VkPipeline pipeline,
                             VkPipelineLayout layout,
                             VkShaderStageFlags pushStages,
                             std::string debugName);
    ~IXVulkanGraphicsPipeline() override;

    const std::string& DebugName() const override { return m_debugName; }
    VkPipeline Native() const { return m_pipeline; }
    VkPipelineLayout NativeLayout() const { return m_layout; }
    VkShaderStageFlags PushStages() const { return m_pushStages; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkShaderStageFlags m_pushStages = 0;
    std::string m_debugName;
};

class IXVulkanComputePipeline final : public ixrhi::IXRHIComputePipeline
{
public:
    IXVulkanComputePipeline(IXVulkanDevice& device,
                            VkPipeline pipeline,
                            VkPipelineLayout layout,
                            VkShaderStageFlags pushStages,
                            std::string debugName);
    ~IXVulkanComputePipeline() override;

    const std::string& DebugName() const override { return m_debugName; }
    VkPipeline Native() const { return m_pipeline; }
    VkPipelineLayout NativeLayout() const { return m_layout; }
    VkShaderStageFlags PushStages() const { return m_pushStages; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkShaderStageFlags m_pushStages = 0;
    std::string m_debugName;
};

} // namespace ixvulkan
