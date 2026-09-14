#pragma once

// IXVulkanConversions — centralized IXRHI <-> Vulkan translation (Phase 2, §38).
// ALL format/enum switches live here; renderer and pipeline classes call these.
// Pure functions over values (no device state) so IXRHISmoke unit-tests them
// without a GPU. Unknown/unsupported inputs map to explicit safe fallbacks and
// return false from Try* variants where renderer code must branch.

#include "IXRHITypes.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace ixvulkan
{

VkFormat ToVkFormat(ixrhi::IXRHIFormat format);
ixrhi::IXRHIFormat FromVkFormat(VkFormat format);

VkBufferUsageFlags ToVkBufferUsage(ixrhi::IXRHIBufferUsage usage);
VkImageUsageFlags ToVkImageUsage(ixrhi::IXRHITextureUsage usage);
VkShaderStageFlags ToVkShaderStages(ixrhi::IXRHIShaderStage stages);
VkPrimitiveTopology ToVkTopology(ixrhi::IXRHIPrimitiveTopology topology);
VkCullModeFlags ToVkCullMode(ixrhi::IXRHICullMode mode);
VkFrontFace ToVkFrontFace(ixrhi::IXRHIFrontFace face);
VkCompareOp ToVkCompareOp(ixrhi::IXRHICompareOp op);
VkBlendFactor ToVkBlendFactor(ixrhi::IXRHIBlendFactor factor);
VkBlendOp ToVkBlendOp(ixrhi::IXRHIBlendOp op);
VkDescriptorType ToVkDescriptorType(ixrhi::IXRHIBindingType type);
VkFilter ToVkFilter(ixrhi::IXRHISamplerFilter filter);
VkSamplerAddressMode ToVkAddressMode(ixrhi::IXRHISamplerAddress mode);
VkImageLayout ToVkImageLayout(ixrhi::IXRHIImageLayout layout);
VkSampleCountFlagBits ToVkSampleCount(std::uint32_t count);

} // namespace ixvulkan
