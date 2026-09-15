#pragma once

// IXRHIBuffer — GPU buffer contract. Renderer never manages Vulkan buffer memory.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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

// Non-blocking device-local upload (Phase 3A: LOD index-buffer streaming).
// The backend stages through host memory and submits the copy WITHOUT waiting;
// IsReady() polls completion. Take() returns the finished buffer (null until
// ready) and consumes the upload. Destruction before Take() waits and releases
// everything — safe to drop on renderer teardown, like waiting the fence in
// the pre-migration Destroy().
class IXRHIBufferUpload
{
public:
    virtual ~IXRHIBufferUpload() = default;

    virtual bool IsReady() = 0;
    virtual std::shared_ptr<IXRHIBuffer> Take() = 0;
};

} // namespace ixrhi
