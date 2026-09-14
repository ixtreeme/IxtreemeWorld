// IXVulkan bind-group objects + their device factory calls.

#include "IXVulkanBinding.h"

#include "IXVulkanConversions.h"
#include "IXVulkanDevice.h"
#include "IXVulkanResources.h"

namespace ixvulkan
{

IXVulkanBindGroupLayout::IXVulkanBindGroupLayout(IXVulkanDevice& device,
                                                 VkDescriptorSetLayout layout,
                                                 std::vector<ixrhi::IXRHIBinding> bindings)
    : m_device(&device)
    , m_layout(layout)
    , m_bindings(std::move(bindings))
{
}

IXVulkanBindGroupLayout::~IXVulkanBindGroupLayout()
{
    if (m_device != nullptr && m_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(m_device->NativeDevice(), m_layout, nullptr);
}

IXVulkanBindGroup::IXVulkanBindGroup(IXVulkanDevice& device,
                                     VkDescriptorPool pool,
                                     std::vector<VkDescriptorSet> sets,
                                     std::vector<std::shared_ptr<ixrhi::IXRHIBuffer>> bufferKeeps,
                                     std::vector<std::shared_ptr<ixrhi::IXRHITexture>> textureKeeps,
                                     std::vector<std::shared_ptr<ixrhi::IXRHISampler>> samplerKeeps)
    : m_device(&device)
    , m_pool(pool)
    , m_sets(std::move(sets))
    , m_buffers(std::move(bufferKeeps))
    , m_textures(std::move(textureKeeps))
    , m_samplers(std::move(samplerKeeps))
{
}

IXVulkanBindGroup::~IXVulkanBindGroup()
{
    // Sets are implicitly freed with the pool (pool created without FREE_BIT,
    // same as pre-migration renderers).
    if (m_device != nullptr && m_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_device->NativeDevice(), m_pool, nullptr);
}

void IXVulkanBindGroup::UpdateBuffer(std::uint32_t setIndex,
                                     std::uint32_t binding,
                                     std::shared_ptr<ixrhi::IXRHIBuffer> buffer,
                                     std::uint64_t offsetBytes,
                                     std::uint64_t rangeBytes)
{
    if (setIndex >= m_sets.size() || !buffer)
        return;
    auto* native = dynamic_cast<IXVulkanBuffer*>(buffer.get());
    if (native == nullptr)
        return;
    m_buffers.push_back(buffer); // shared lifetime with the group

    VkDescriptorBufferInfo info{};
    info.buffer = native->Native();
    info.offset = static_cast<VkDeviceSize>(offsetBytes);
    info.range = static_cast<VkDeviceSize>(rangeBytes);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_sets[setIndex];
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    // Storage vs uniform distinguished at layout time; the write type must match
    // the layout binding. Resolve via layout is unavailable here, so match the
    // buffer usage (uniform buffers are the Phase-2 case; storage handled below).
    if ((static_cast<std::uint32_t>(buffer->Usage()) &
            static_cast<std::uint32_t>(ixrhi::IXRHIBufferUsage::Storage)) != 0)
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vkUpdateDescriptorSets(m_device->NativeDevice(), 1, &write, 0, nullptr);
}

void IXVulkanBindGroup::UpdateTexture(std::uint32_t setIndex,
                                      std::uint32_t binding,
                                      std::shared_ptr<ixrhi::IXRHITexture> texture,
                                      std::shared_ptr<ixrhi::IXRHISampler> sampler)
{
    if (setIndex >= m_sets.size() || !texture || !sampler)
        return;
    auto* nativeTexture = dynamic_cast<IXVulkanTexture*>(texture.get());
    auto* nativeSampler = dynamic_cast<IXVulkanSampler*>(sampler.get());
    if (nativeTexture == nullptr || nativeSampler == nullptr)
        return;
    m_textures.push_back(texture);
    m_samplers.push_back(sampler);

    VkDescriptorImageInfo info{};
    info.sampler = nativeSampler->Native();
    info.imageView = nativeTexture->NativeView();
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_sets[setIndex];
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device->NativeDevice(), 1, &write, 0, nullptr);
}

VkDescriptorSet IXVulkanBindGroup::NativeSet(std::uint32_t setIndex) const
{
    return setIndex < m_sets.size() ? m_sets[setIndex] : VK_NULL_HANDLE;
}

std::unique_ptr<ixrhi::IXRHIBindGroupLayout> IXVulkanDevice::CreateBindGroupLayout(
    const std::vector<ixrhi::IXRHIBinding>& bindings)
{
    if (bindings.empty())
        return nullptr;
    std::vector<VkDescriptorSetLayoutBinding> native;
    native.reserve(bindings.size());
    for (const ixrhi::IXRHIBinding& binding : bindings)
    {
        VkDescriptorSetLayoutBinding entry{};
        entry.binding = binding.binding;
        entry.descriptorType = ToVkDescriptorType(binding.type);
        entry.descriptorCount = 1;
        entry.stageFlags = ToVkShaderStages(binding.stages);
        native.push_back(entry);
    }
    VkDescriptorSetLayoutCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create.bindingCount = static_cast<std::uint32_t>(native.size());
    create.pBindings = native.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    CheckVk(vkCreateDescriptorSetLayout(NativeDevice(), &create, nullptr, &layout),
        "vkCreateDescriptorSetLayout",
        __FILE__,
        __LINE__);
    return std::make_unique<IXVulkanBindGroupLayout>(*this, layout, bindings);
}

std::unique_ptr<ixrhi::IXRHIBindGroup> IXVulkanDevice::CreateBindGroup(
    const ixrhi::IXRHIBindGroupLayout& layout, std::uint32_t maxSets)
{
    auto* nativeLayout = dynamic_cast<const IXVulkanBindGroupLayout*>(&layout);
    if (nativeLayout == nullptr || maxSets == 0)
        return nullptr;

    std::vector<VkDescriptorPoolSize> sizes;
    for (const ixrhi::IXRHIBinding& binding : nativeLayout->Bindings())
    {
        bool merged = false;
        for (VkDescriptorPoolSize& size : sizes)
        {
            if (size.type == ToVkDescriptorType(binding.type))
            {
                size.descriptorCount += maxSets;
                merged = true;
                break;
            }
        }
        if (!merged)
            sizes.push_back(VkDescriptorPoolSize{ToVkDescriptorType(binding.type), maxSets});
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckVk(vkCreateDescriptorPool(NativeDevice(), &poolInfo, nullptr, &pool),
        "vkCreateDescriptorPool",
        __FILE__,
        __LINE__);

    std::vector<VkDescriptorSetLayout> layouts(maxSets, nativeLayout->Native());
    std::vector<VkDescriptorSet> sets(maxSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = maxSets;
    alloc.pSetLayouts = layouts.data();
    CheckVk(vkAllocateDescriptorSets(NativeDevice(), &alloc, sets.data()),
        "vkAllocateDescriptorSets",
        __FILE__,
        __LINE__);

    return std::make_unique<IXVulkanBindGroup>(*this,
        pool,
        std::move(sets),
        std::vector<std::shared_ptr<ixrhi::IXRHIBuffer>>{},
        std::vector<std::shared_ptr<ixrhi::IXRHITexture>>{},
        std::vector<std::shared_ptr<ixrhi::IXRHISampler>>{});
}

} // namespace ixvulkan
