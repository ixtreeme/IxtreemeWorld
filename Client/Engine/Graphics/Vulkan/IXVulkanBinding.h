#pragma once

// IXVulkan bind-group objects: layout owns VkDescriptorSetLayout; group owns its
// VkDescriptorPool + sets and writes descriptors on Update* (same pattern and
// same update timing as the pre-migration renderers).

#include "IXRHIBinding.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace ixvulkan
{

class IXVulkanDevice;

class IXVulkanBindGroupLayout final : public ixrhi::IXRHIBindGroupLayout
{
public:
    IXVulkanBindGroupLayout(IXVulkanDevice& device,
                            VkDescriptorSetLayout layout,
                            std::vector<ixrhi::IXRHIBinding> bindings);
    ~IXVulkanBindGroupLayout() override;

    VkDescriptorSetLayout Native() const { return m_layout; }
    const std::vector<ixrhi::IXRHIBinding>& Bindings() const { return m_bindings; }

private:
    IXVulkanDevice* m_device = nullptr;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    std::vector<ixrhi::IXRHIBinding> m_bindings;
};

class IXVulkanBindGroup final : public ixrhi::IXRHIBindGroup
{
public:
    IXVulkanBindGroup(IXVulkanDevice& device,
                      VkDescriptorPool pool,
                      std::vector<VkDescriptorSet> sets,
                      std::vector<std::shared_ptr<ixrhi::IXRHIBuffer>> bufferKeeps,
                      std::vector<std::shared_ptr<ixrhi::IXRHITexture>> textureKeeps,
                      std::vector<std::shared_ptr<ixrhi::IXRHISampler>> samplerKeeps);
    ~IXVulkanBindGroup() override;

    void UpdateBuffer(std::uint32_t setIndex,
                      std::uint32_t binding,
                      std::shared_ptr<ixrhi::IXRHIBuffer> buffer,
                      std::uint64_t offsetBytes,
                      std::uint64_t rangeBytes) override;
    void UpdateTexture(std::uint32_t setIndex,
                       std::uint32_t binding,
                       std::shared_ptr<ixrhi::IXRHITexture> texture,
                       std::shared_ptr<ixrhi::IXRHISampler> sampler) override;
    void UpdateSampledImage(std::uint32_t setIndex,
                            std::uint32_t binding,
                            std::shared_ptr<ixrhi::IXRHITexture> texture) override;
    void UpdateSampler(std::uint32_t setIndex,
                       std::uint32_t binding,
                       std::shared_ptr<ixrhi::IXRHISampler> sampler) override;

    // Set index within this group (groups are created with maxSets slots).
    VkDescriptorSet NativeSet(std::uint32_t setIndex) const;

private:
    IXVulkanDevice* m_device = nullptr;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;
    // Shared lifetime: bound resources outlive any in-flight use of the group. Each resource is kept
    // once (m_kept): groups are rebound every frame (per-draw texture slots), and appending on every
    // update grew these without bound.
    template <typename T>
    void Keep(std::vector<std::shared_ptr<T>>& keeps, const std::shared_ptr<T>& resource)
    {
        if (m_kept.insert(resource.get()).second)
            keeps.push_back(resource);
    }
    std::vector<std::shared_ptr<ixrhi::IXRHIBuffer>> m_buffers;
    std::vector<std::shared_ptr<ixrhi::IXRHITexture>> m_textures;
    std::vector<std::shared_ptr<ixrhi::IXRHISampler>> m_samplers;
    std::unordered_set<const void*> m_kept;
};

} // namespace ixvulkan
