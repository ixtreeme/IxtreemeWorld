#pragma once

// IXRHIShader — consumes the CURRENT compiled representation (SPIR-V from DXC,
// via IAssetReader), not a redesigned language. Source -> ShaderCooker -> SPIR-V
// -> Vulkan stays the future path; MoltenVK consumes the same SPIR-V later.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ixrhi
{

struct IXRHIShaderDesc
{
    IXRHIShaderStage stage = IXRHIShaderStage::None;
    std::string entryPoint = "main"; // this engine uses VSMain/PSMain/CSMain
    std::vector<std::uint32_t> spirv; // DXC output words
    std::string debugName;
};

class IXRHIShader
{
public:
    virtual ~IXRHIShader() = default;

    virtual IXRHIShaderStage Stage() const = 0;
    virtual const std::string& EntryPoint() const = 0;
    virtual const std::string& DebugName() const = 0;
};

} // namespace ixrhi
