#pragma once

// IXRHITexture / IXRHISampler contracts. Backend owns image, memory, view(s),
// layout bookkeeping and sampler; renderer code never sees VkImage/View/Sampler.
//
// Resources are always shared_ptr-owned (see IXRHIDevice ownership contract),
// so bases enable shared_from_this for registry keep-alives.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ixrhi
{

struct IXRHITextureDesc
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1;
    IXRHIFormat format = IXRHIFormat::Undefined;
    IXRHITextureUsage usage = IXRHITextureUsage::None;
    std::uint32_t sampleCount = 1;
    // Cube map: arrayLayers must be 6 (faces +X, -X, +Y, -Y, +Z, -Z) and width == height; the
    // texture is then sampled as a TextureCube. D3D12: TEXTURECUBE SRV — natural.
    bool cubeMap = false;
    std::string debugName;
};

// Initial-data packing for CreateTexture with mipLevels > 1 or
// arrayLayers > 1 (terrain material arrays): tightly packed, layer-major,
// mip-minor — layer 0 mip 0, layer 0 mip 1, ..., layer 1 mip 0, ... — each
// subresource tightly packed (mipWidth * mipHeight * pixelBytes, halving
// per level clamped to 1, no row pitch). Vulkan: one buffer-image region
// per subresource; D3D12: placed footprints per subresource — natural.
class IXRHITexture : public std::enable_shared_from_this<IXRHITexture>
{
public:
    virtual ~IXRHITexture() = default;

    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
    virtual std::uint32_t MipLevels() const = 0;
    virtual std::uint32_t ArrayLayers() const = 0;
    virtual IXRHIFormat Format() const = 0;
    virtual const std::string& DebugName() const = 0;
    // Dedicated image allocation, including backend alignment; zero when unavailable or borrowed.
    virtual std::uint64_t AllocatedBytes() const { return 0; }
};

struct IXRHISamplerDesc
{
    IXRHISamplerFilter minFilter = IXRHISamplerFilter::Linear;
    IXRHISamplerFilter magFilter = IXRHISamplerFilter::Linear;
    IXRHISamplerFilter mipmapFilter = IXRHISamplerFilter::Nearest;
    IXRHISamplerAddress addressU = IXRHISamplerAddress::ClampToEdge;
    IXRHISamplerAddress addressV = IXRHISamplerAddress::ClampToEdge;
    IXRHISamplerAddress addressW = IXRHISamplerAddress::ClampToEdge;
    float maxLod = 1.0f;
    float mipLodBias = 0.0f;
    std::uint32_t maxAnisotropy = 1; // >1 requires capabilities.supportsAnisotropy
    // Depth-compare sampling (shadow maps). D3D12: COMPARISON filter +
    // ComparisonFunc — natural. MoltenVK: compare samplers supported.
    bool compareEnable = false;
    IXRHICompareOp compareOp = IXRHICompareOp::LessOrEqual;
    std::string debugName;
};

class IXRHISampler : public std::enable_shared_from_this<IXRHISampler>
{
public:
    virtual ~IXRHISampler() = default;
    virtual const std::string& DebugName() const = 0;
};

} // namespace ixrhi
