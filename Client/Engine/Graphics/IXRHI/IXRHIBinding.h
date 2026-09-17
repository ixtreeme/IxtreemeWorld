#pragma once

// IXRHI resource binding: engine-level bind groups. No VkDescriptorSetLayout,
// VkDescriptorPool or VkDescriptorSet outside the Vulkan backend.
//
// Model: layout describes slots (binding/type/stages); group is allocated from
// the layout and updated with buffer or texture+sampler references. Groups hold
// shared_ptr to the bound resources so per-frame buffers stay alive as long as
// any submitted command list may reference them.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ixrhi
{

struct IXRHIBinding
{
    std::uint32_t binding = 0;
    IXRHIBindingType type = IXRHIBindingType::UniformBuffer;
    IXRHIShaderStage stages = IXRHIShaderStage::None;
};

class IXRHIBindGroupLayout
{
public:
    virtual ~IXRHIBindGroupLayout() = default;
};

class IXRHIBindGroup
{
public:
    virtual ~IXRHIBindGroup() = default;

    // setIndex selects one of the maxSets slots from CreateBindGroup (typically
    // the frame index for per-frame uniform buffers).
    virtual void UpdateBuffer(std::uint32_t setIndex,
                              std::uint32_t binding,
                              std::shared_ptr<IXRHIBuffer> buffer,
                              std::uint64_t offsetBytes,
                              std::uint64_t rangeBytes) = 0;

    virtual void UpdateTexture(std::uint32_t setIndex,
                               std::uint32_t binding,
                               std::shared_ptr<IXRHITexture> texture,
                               std::shared_ptr<IXRHISampler> sampler) = 0;

    // Separate image / sampler writes for SampledImage / Sampler bindings
    // (shaders declaring texture and sampler apart, e.g. RmlUi). The group
    // keeps both alive like the combined path.
    virtual void UpdateSampledImage(std::uint32_t setIndex,
                                    std::uint32_t binding,
                                    std::shared_ptr<IXRHITexture> texture) = 0;

    virtual void UpdateSampler(std::uint32_t setIndex,
                               std::uint32_t binding,
                               std::shared_ptr<IXRHISampler> sampler) = 0;
};

} // namespace ixrhi
