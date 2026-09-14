#pragma once

// IXRHI pipeline contracts. Enough state for the real migrated renderers; no
// Vulkan structs cross this boundary (no VkRenderPass coupling — see below).
//
// Render-pass strategy while the backend is VkRenderPass-based (no dynamic
// rendering in tree): the backend resolves a compatible VkRenderPass internally
// (override set by the frame owner, else the swapchain pass). The desc carries
// color/depth formats so Phase 3 can switch the backend to dynamic rendering
// WITHOUT changing renderer code.

#include "IXRHI.h"
#include "IXRHIBinding.h"
#include "IXRHITypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ixrhi
{

struct IXRHIVertexBinding
{
    std::uint32_t binding = 0;
    std::uint32_t strideBytes = 0;
};

struct IXRHIVertexAttribute
{
    std::uint32_t location = 0;
    std::uint32_t binding = 0;
    IXRHIFormat format = IXRHIFormat::Undefined;
    std::uint32_t offsetBytes = 0;
};

struct IXRHIBlendAttachment
{
    bool blendEnable = false;
    IXRHIBlendFactor srcColor = IXRHIBlendFactor::One;
    IXRHIBlendFactor dstColor = IXRHIBlendFactor::Zero;
    IXRHIBlendOp colorOp = IXRHIBlendOp::Add;
    IXRHIBlendFactor srcAlpha = IXRHIBlendFactor::One;
    IXRHIBlendFactor dstAlpha = IXRHIBlendFactor::Zero;
    IXRHIBlendOp alphaOp = IXRHIBlendOp::Add;
};

struct IXRHIPushRange
{
    IXRHIShaderStage stages = IXRHIShaderStage::None;
    std::uint32_t offsetBytes = 0;
    std::uint32_t sizeBytes = 0;
};

struct IXRHIGraphicsPipelineDesc
{
    std::shared_ptr<IXRHIShader> vertexShader;
    std::shared_ptr<IXRHIShader> fragmentShader;
    std::vector<const IXRHIBindGroupLayout*> bindGroupLayouts;
    std::vector<IXRHIPushRange> pushRanges;
    std::vector<IXRHIVertexBinding> vertexBindings;
    std::vector<IXRHIVertexAttribute> vertexAttributes;
    IXRHIPrimitiveTopology topology = IXRHIPrimitiveTopology::TriangleList;
    IXRHICullMode cullMode = IXRHICullMode::None;
    IXRHIFrontFace frontFace = IXRHIFrontFace::CounterClockwise;
    bool depthTestEnable = false;
    bool depthWriteEnable = false;
    IXRHICompareOp depthCompareOp = IXRHICompareOp::Less;
    std::vector<IXRHIBlendAttachment> blendAttachments; // one per color target
    std::vector<IXRHIFormat> colorFormats; // informational until dynamic rendering
    IXRHIFormat depthFormat = IXRHIFormat::Undefined;
    std::uint32_t sampleCount = 1;
    std::string debugName;
};

class IXRHIGraphicsPipeline
{
public:
    virtual ~IXRHIGraphicsPipeline() = default;
    virtual const std::string& DebugName() const = 0;
};

struct IXRHIComputePipelineDesc
{
    std::shared_ptr<IXRHIShader> computeShader;
    std::vector<const IXRHIBindGroupLayout*> bindGroupLayouts;
    std::vector<IXRHIPushRange> pushRanges;
    std::string debugName;
};

class IXRHIComputePipeline
{
public:
    virtual ~IXRHIComputePipeline() = default;
    virtual const std::string& DebugName() const = 0;
};

} // namespace ixrhi
