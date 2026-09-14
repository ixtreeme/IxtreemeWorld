#pragma once

// IXRHI descriptor types (Phase 1: reserved, not yet consumed by renderers).
// Mirrors the canonical list from the architecture brief so Phase 2 does not have
// to rename anything: IXRHIBufferDesc / IXRHITextureDesc / IXRHIShaderDesc /
// IXRHIGraphicsPipelineDesc / IXRHIComputePipelineDesc / IXRHISamplerDesc.

#include "IXRHI.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ixrhi
{

enum class IXRHIBufferUsage : std::uint32_t
{
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Staging = 1u << 4,
};

enum class IXRHITextureUsage : std::uint32_t
{
    Sampled = 1u << 0,
    ColorAttachment = 1u << 1,
    DepthAttachment = 1u << 2,
    Storage = 1u << 3,
};

struct IXRHIBufferDesc
{
    std::uint64_t sizeBytes = 0;
    std::uint32_t usageMask = 0;
    bool hostVisible = false;
};

struct IXRHITextureDesc
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t usageMask = 0;
    const char* formatHint = nullptr; // e.g. "BC7", "RGBA8", "D32F" — backend maps it
};

struct IXRHIShaderDesc
{
    std::string entryPoint = "main";
    std::vector<std::uint32_t> spirv; // DXC output today; Phase 2 keeps this path
};

struct IXRHIGraphicsPipelineDesc
{
    IXRHIShaderHandle vertexShader;
    IXRHIShaderHandle pixelShader;
};

struct IXRHIComputePipelineDesc
{
    IXRHIShaderHandle computeShader; // e.g. SkinnedMeshSkin CSMain skinning kernel
};

struct IXRHISamplerDesc
{
    bool linearFilter = true;
    std::uint32_t maxAnisotropy = 1;
};

} // namespace ixrhi
