#pragma once

// IXRHICommandList — recording-only command interface used INSIDE an active
// frame (IXRHI-owned command list from the frame context) or render target.
// Renderers must issue all draw-state commands through this; no vkCmd*
// outside the backend.
//
// Begin()/End() bracket OWNED lists (device.CreateCommandList). Borrowed frame
// lists (frame context commandList) are already recording: Begin/End are
// accepted no-ops so renderer code is identical for both.

#include "IXRHI.h"
#include "IXRHITypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ixrhi
{

class IXRHITexture;

class IXRHICommandList
{
public:
    virtual ~IXRHICommandList() = default;

    virtual void Begin() = 0;
    virtual void End() = 0;

    virtual void SetViewport(float x, float y, float width, float height) = 0;
    virtual void SetScissor(std::uint32_t x,
                            std::uint32_t y,
                            std::uint32_t width,
                            std::uint32_t height) = 0;

    virtual void SetGraphicsPipeline(const IXRHIGraphicsPipeline& pipeline) = 0;

    virtual void SetVertexBuffer(std::uint32_t slot,
                                 const IXRHIBuffer& buffer,
                                 std::uint64_t offsetBytes) = 0;

    virtual void SetIndexBuffer(const IXRHIBuffer& buffer,
                                std::uint64_t offsetBytes,
                                bool thirtyTwoBit) = 0;

    // layoutSet: set number in the pipeline layout. slotIndex: one of the
    // maxSets slots of the group (typically the frame index for per-frame sets).
    virtual void BindGroup(std::uint32_t layoutSet,
                           const IXRHIBindGroup& group,
                           std::uint32_t slotIndex) = 0;

    virtual void PushConstants(const void* data, std::size_t byteCount) = 0;

    virtual void Draw(std::uint32_t vertexCount,
                      std::uint32_t instanceCount = 1,
                      std::uint32_t firstVertex = 0,
                      std::uint32_t firstInstance = 0) = 0;

    virtual void DrawIndexed(std::uint32_t indexCount,
                             std::uint32_t instanceCount = 1,
                             std::uint32_t firstIndex = 0,
                             std::int32_t vertexOffset = 0,
                             std::uint32_t firstInstance = 0) = 0;

    virtual void Dispatch(std::uint32_t groupsX,
                          std::uint32_t groupsY,
                          std::uint32_t groupsZ) = 0;

    // Explicit layout transition (backend inserts the barrier). The caller
    // tracks states (as the offscreen snapshot flow does); the backend maps
    // (from, to, format) to stages/access uniformly — no per-use barrier DSL.
    virtual void TransitionTexture(IXRHITexture& texture,
                                   IXRHIImageLayout from,
                                   IXRHIImageLayout to) = 0;

    // Full-subresource same-size copy. Both textures must already be in
    // TransferSrc (src) / TransferDst (dst); aspects derive from formats.
    virtual void CopyTexture(const IXRHITexture& src, IXRHITexture& dst) = 0;
};

} // namespace ixrhi
