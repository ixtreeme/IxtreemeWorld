// Conversion round-trip checks (need <vulkan/vulkan.h> as DATA only — the IXRHI
// contract headers above it stay Vulkan-free). Registered in the same binary so
// `ctest -R IXRHISmoke` covers contract + backend mapping together.

#include "IXVulkanConversions.h"

#include <cstdio>

extern int g_ixrhiSmokeFailures;

namespace
{

void CheckConv(bool condition, const char* name)
{
    if (condition)
    {
        std::printf("[PASS] %s\n", name);
        return;
    }
    ++g_ixrhiSmokeFailures;
    std::printf("[FAIL] %s\n", name);
}

} // namespace

void RunConversionChecks()
{
    using ixrhi::IXRHIFormat;
    // Format round-trips through the backend mapping.
    CheckConv(ixvulkan::ToVkFormat(IXRHIFormat::R8G8B8A8Unorm) == VK_FORMAT_R8G8B8A8_UNORM,
        "conv RGBA8 -> Vk");
    CheckConv(ixvulkan::FromVkFormat(VK_FORMAT_R8G8B8A8_UNORM) == IXRHIFormat::R8G8B8A8Unorm,
        "conv RGBA8 <- Vk");
    CheckConv(ixvulkan::ToVkFormat(IXRHIFormat::R32G32B32Float) == VK_FORMAT_R32G32B32_SFLOAT,
        "conv Float3 -> Vk");
    CheckConv(ixvulkan::ToVkFormat(IXRHIFormat::R32G32B32A32Float) ==
            VK_FORMAT_R32G32B32A32_SFLOAT,
        "conv Float4 -> Vk");
    CheckConv(ixvulkan::FromVkFormat(VK_FORMAT_B8G8R8A8_SRGB) == IXRHIFormat::B8G8R8A8Srgb,
        "conv swapchain sRGB <- Vk");
    CheckConv(ixvulkan::FromVkFormat(VK_FORMAT_D32_SFLOAT) == IXRHIFormat::D32Float,
        "conv depth <- Vk");
    CheckConv(ixvulkan::ToVkFormat(IXRHIFormat::Undefined) == VK_FORMAT_UNDEFINED,
        "conv Undefined fallback");
    CheckConv(ixvulkan::FromVkFormat(VK_FORMAT_ASTC_8x8_UNORM_BLOCK) == IXRHIFormat::Undefined,
        "conv exotic -> Undefined");

    // Usage masks compose.
    const auto usage = ixrhi::IXRHIBufferUsage::Vertex | ixrhi::IXRHIBufferUsage::TransferDst;
    const VkBufferUsageFlags vkUsage = ixvulkan::ToVkBufferUsage(usage);
    CheckConv((vkUsage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0, "conv usage vertex bit");
    CheckConv((vkUsage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0, "conv usage xfer bit");
    CheckConv((vkUsage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) == 0, "conv usage negative");
    CheckConv(ixvulkan::ToVkBufferUsage(ixrhi::IXRHIBufferUsage::Uniform) ==
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        "conv usage uniform");

    const auto texUsage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    const VkImageUsageFlags vkTex = ixvulkan::ToVkImageUsage(texUsage);
    CheckConv((vkTex & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
            (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        "conv texture usage");

    // State enums hit the exact pre-migration values.
    CheckConv(ixvulkan::ToVkTopology(ixrhi::IXRHIPrimitiveTopology::LineList) ==
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        "conv topology lines");
    CheckConv(ixvulkan::ToVkCullMode(ixrhi::IXRHICullMode::None) == VK_CULL_MODE_NONE,
        "conv cull none");
    CheckConv(ixvulkan::ToVkCompareOp(ixrhi::IXRHICompareOp::LessOrEqual) ==
            VK_COMPARE_OP_LESS_OR_EQUAL,
        "conv compare LEQUAL");
    CheckConv(ixvulkan::ToVkBlendFactor(ixrhi::IXRHIBlendFactor::SrcAlpha) ==
            VK_BLEND_FACTOR_SRC_ALPHA,
        "conv blend srcAlpha");
    CheckConv(ixvulkan::ToVkDescriptorType(ixrhi::IXRHIBindingType::SampledTexture) ==
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        "conv binding sampler");
    CheckConv(ixvulkan::ToVkImageLayout(ixrhi::IXRHIImageLayout::ShaderReadOnly) ==
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "conv layout read-only");
    CheckConv(ixvulkan::ToVkSampleCount(1) == VK_SAMPLE_COUNT_1_BIT, "conv samples 1");
}
