// Centralized IXRHI -> Vulkan translation. See header for contract.

#include "IXVulkanConversions.h"

#include "IXRHIFrame.h"

namespace ixvulkan
{

VkFormat ToVkFormat(ixrhi::IXRHIFormat format)
{
    using F = ixrhi::IXRHIFormat;
    switch (format)
    {
    case F::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case F::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case F::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case F::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    case F::R32G32B32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case F::R32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case F::D32Float: return VK_FORMAT_D32_SFLOAT;
    case F::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case F::Undefined: break;
    }
    return VK_FORMAT_UNDEFINED;
}

ixrhi::IXRHIFormat FromVkFormat(VkFormat format)
{
    using F = ixrhi::IXRHIFormat;
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return F::R8G8B8A8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return F::R8G8B8A8Srgb;
    case VK_FORMAT_B8G8R8A8_UNORM: return F::B8G8R8A8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return F::B8G8R8A8Srgb;
    case VK_FORMAT_R32G32_SFLOAT: return F::R32G32Float;
    case VK_FORMAT_R32G32B32_SFLOAT: return F::R32G32B32Float;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return F::R32G32B32A32Float;
    case VK_FORMAT_D32_SFLOAT: return F::D32Float;
    case VK_FORMAT_D24_UNORM_S8_UINT: return F::D24UnormS8Uint;
    default: break;
    }
    return F::Undefined;
}

VkBufferUsageFlags ToVkBufferUsage(ixrhi::IXRHIBufferUsage usage)
{
    using U = ixrhi::IXRHIBufferUsage;
    VkBufferUsageFlags flags = 0;
    const auto bits = static_cast<std::uint32_t>(usage);
    if (bits & static_cast<std::uint32_t>(U::Vertex))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (bits & static_cast<std::uint32_t>(U::Index))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (bits & static_cast<std::uint32_t>(U::Uniform))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (bits & static_cast<std::uint32_t>(U::Storage))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferSrc))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferDst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (bits & static_cast<std::uint32_t>(U::Indirect))
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return flags;
}

VkImageUsageFlags ToVkImageUsage(ixrhi::IXRHITextureUsage usage)
{
    using U = ixrhi::IXRHITextureUsage;
    VkImageUsageFlags flags = 0;
    const auto bits = static_cast<std::uint32_t>(usage);
    if (bits & static_cast<std::uint32_t>(U::Sampled))
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (bits & static_cast<std::uint32_t>(U::ColorAttachment))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (bits & static_cast<std::uint32_t>(U::DepthStencilAttachment))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferDst))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (bits & static_cast<std::uint32_t>(U::TransferSrc))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return flags;
}

VkShaderStageFlags ToVkShaderStages(ixrhi::IXRHIShaderStage stages)
{
    using S = ixrhi::IXRHIShaderStage;
    VkShaderStageFlags flags = 0;
    const auto bits = static_cast<std::uint32_t>(stages);
    if (bits & static_cast<std::uint32_t>(S::Vertex))
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (bits & static_cast<std::uint32_t>(S::Fragment))
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (bits & static_cast<std::uint32_t>(S::Compute))
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    return flags;
}

VkPrimitiveTopology ToVkTopology(ixrhi::IXRHIPrimitiveTopology topology)
{
    using T = ixrhi::IXRHIPrimitiveTopology;
    switch (topology)
    {
    case T::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case T::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case T::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags ToVkCullMode(ixrhi::IXRHICullMode mode)
{
    using C = ixrhi::IXRHICullMode;
    switch (mode)
    {
    case C::None: return VK_CULL_MODE_NONE;
    case C::Front: return VK_CULL_MODE_FRONT_BIT;
    case C::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkFrontFace ToVkFrontFace(ixrhi::IXRHIFrontFace face)
{
    return face == ixrhi::IXRHIFrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                    : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkCompareOp ToVkCompareOp(ixrhi::IXRHICompareOp op)
{
    using C = ixrhi::IXRHICompareOp;
    switch (op)
    {
    case C::Never: return VK_COMPARE_OP_NEVER;
    case C::Less: return VK_COMPARE_OP_LESS;
    case C::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case C::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

VkBlendFactor ToVkBlendFactor(ixrhi::IXRHIBlendFactor factor)
{
    using B = ixrhi::IXRHIBlendFactor;
    switch (factor)
    {
    case B::Zero: return VK_BLEND_FACTOR_ZERO;
    case B::One: return VK_BLEND_FACTOR_ONE;
    case B::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case B::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVkBlendOp(ixrhi::IXRHIBlendOp op)
{
    (void)op;
    return VK_BLEND_OP_ADD;
}

VkDescriptorType ToVkDescriptorType(ixrhi::IXRHIBindingType type)
{
    using B = ixrhi::IXRHIBindingType;
    switch (type)
    {
    case B::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case B::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case B::SampledTexture: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case B::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case B::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkFilter ToVkFilter(ixrhi::IXRHISamplerFilter filter)
{
    return filter == ixrhi::IXRHISamplerFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode ToVkAddressMode(ixrhi::IXRHISamplerAddress mode)
{
    return mode == ixrhi::IXRHISamplerAddress::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                      : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkImageLayout ToVkImageLayout(ixrhi::IXRHIImageLayout layout)
{
    using L = ixrhi::IXRHIImageLayout;
    switch (layout)
    {
    case L::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case L::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case L::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case L::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case L::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case L::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case L::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkSampleCountFlagBits ToVkSampleCount(std::uint32_t count)
{
    switch (count)
    {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    default: break;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VkImageAspectFlags ToVkAspectMask(ixrhi::IXRHIFormat format)
{
    using F = ixrhi::IXRHIFormat;
    switch (format)
    {
    case F::D32Float: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case F::D24UnormS8Uint: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: break;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkAttachmentLoadOp ToVkLoadOp(ixrhi::IXRHILoadOp op)
{
    using L = ixrhi::IXRHILoadOp;
    switch (op)
    {
    case L::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case L::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case L::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp ToVkStoreOp(ixrhi::IXRHIStoreOp op)
{
    return op == ixrhi::IXRHIStoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

ixrhi::IXRHIFrameResult TranslateFrameResult(VkResult result)
{
    using R = ixrhi::IXRHIFrameResult;
    switch (result)
    {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR: return R::Success;
    case VK_ERROR_OUT_OF_DATE_KHR: return R::SwapchainRecreated;
    case VK_TIMEOUT:
    case VK_NOT_READY: return R::Skip;
    case VK_ERROR_DEVICE_LOST: return R::DeviceLost;
    default: break;
    }
    return R::DeviceLost;
}

} // namespace ixvulkan
