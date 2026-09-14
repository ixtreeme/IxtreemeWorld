#pragma once

// IXRHIBuffer — GPU buffer contract. Renderer never manages Vulkan buffer memory.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ixrhi
{

struct IXRHIBufferDesc
{
    std::uint64_t sizeBytes = 0;
    IXRHIBufferUsage usage = IXRHIBufferUsage::None;
    IXRHICpuAccess cpuAccess = IXRHICpuAccess::None;
    std::string debugName;
};

class IXRHIBuffer
{
public:
    virtual ~IXRHIBuffer() = default;

    // Host write into a Write-visible buffer (map/memcpy/unmap inside backend;
    // coherent memory, no explicit flush needed by the caller).
    virtual void Write(std::uint64_t dstOffsetBytes, const void* src, std::size_t byteCount) = 0;

    virtual std::uint64_t SizeBytes() const = 0;
    virtual IXRHIBufferUsage Usage() const = 0;
    virtual const std::string& DebugName() const = 0;
};

} // namespace ixrhi
