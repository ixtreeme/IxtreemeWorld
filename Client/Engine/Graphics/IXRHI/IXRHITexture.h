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
    std::string debugName;
};

class IXRHITexture : public std::enable_shared_from_this<IXRHITexture>
{
public:
    virtual ~IXRHITexture() = default;

    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
    virtual IXRHIFormat Format() const = 0;
    virtual const std::string& DebugName() const = 0;
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
    std::uint32_t maxAnisotropy = 1; // >1 requires capabilities.supportsAnisotropy
    std::string debugName;
};

class IXRHISampler : public std::enable_shared_from_this<IXRHISampler>
{
public:
    virtual ~IXRHISampler() = default;
    virtual const std::string& DebugName() const = 0;
};

} // namespace ixrhi
