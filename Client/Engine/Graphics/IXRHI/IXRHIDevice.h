#pragma once

// IXRHIDevice — the engine graphics contract implementation point.
//
// LIFETIME / THREAD RULES (binding):
// - The device outlives every object it creates. Renderers hold IXRHIDevice as a
//   non-owning pointer set in Create(); the frame owner (EngineApplication in
//   Phase 2) destroys renderers before the device (stack order enforces this).
// - Resource creation is safe on any thread while no frame is recording against
//   the objects involved; command recording is render-thread only. The backend
//   provides no hidden locking — matching Vulkan's own guarantees.
// - UploadTexture performs an internally synchronized one-time submit (same as
//   the pre-migration code: submit + queue wait-idle), so it is callable during
//   setup; it MUST NOT be called from inside a recording command list.
//
// PERFORMANCE: virtual dispatch exists at creation/binding granularity (one call
// per buffer/pipeline/draw-state change). There are no per-vertex/per-instance
// virtual calls; draw recording is a handful of vcall per draw — negligible next
// to the draw itself. No measurement-driven reason for a handle+function design.

#include "IXRHI.h"
#include "IXRHIBinding.h"
#include "IXRHIBuffer.h"
#include "IXRHICapabilities.h"
#include "IXRHICommandList.h"
#include "IXRHIPipeline.h"
#include "IXRHIRenderTarget.h"
#include "IXRHIShader.h"
#include "IXRHISwapchain.h"
#include "IXRHISync.h"
#include "IXRHITexture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ixrhi
{

// Snapshot of the in-flight frame, filled by the frame owner from the loop that
// still owns swapchain pacing (VulkanDevice in Phase 2). Replaces the renderer's
// direct GetFrameIndex()/GetSwapchainExtent()/IsFrameActive() calls.
struct IXRHIFrameInfo
{
    std::uint32_t frameIndex = 0;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    bool frameActive = false;
    // Monotonic frame counter from the loop (diagnostics only, not an index).
    std::uint64_t frameNumber = 0;
};

class IXRHIDevice
{
public:
    virtual ~IXRHIDevice() = default;

    virtual std::shared_ptr<IXRHIBuffer> CreateBuffer(const IXRHIBufferDesc& desc,
                                                      const void* initialDataOrNull,
                                                      std::size_t initialBytes) = 0;

    // initialDataOrNull: tightly packed RGBA8 (or format-sized) texels for the
    // full base level, or null for render-target/depth textures. With data, the
    // backend stages through a host buffer and transitions to ShaderReadOnly.
    virtual std::shared_ptr<IXRHITexture> CreateTexture(const IXRHITextureDesc& desc,
                                                        const void* initialDataOrNull,
                                                        std::size_t initialBytes) = 0;

    virtual std::shared_ptr<IXRHISampler> CreateSampler(const IXRHISamplerDesc& desc) = 0;

    virtual std::shared_ptr<IXRHIShader> CreateShader(const IXRHIShaderDesc& desc) = 0;

    virtual std::unique_ptr<IXRHIBindGroupLayout> CreateBindGroupLayout(
        const std::vector<IXRHIBinding>& bindings) = 0;

    virtual std::unique_ptr<IXRHIBindGroup> CreateBindGroup(
        const IXRHIBindGroupLayout& layout,
        std::uint32_t maxSets) = 0;

    virtual std::unique_ptr<IXRHIGraphicsPipeline> CreateGraphicsPipeline(
        const IXRHIGraphicsPipelineDesc& desc) = 0;

    virtual std::unique_ptr<IXRHIComputePipeline> CreateComputePipeline(
        const IXRHIComputePipelineDesc& desc) = 0;

    virtual std::unique_ptr<IXRHICommandList> CreateCommandList() = 0;

    // Staged device-local upload without blocking the caller (see
    // IXRHIBufferUpload). Data is copied into backend staging immediately.
    virtual std::unique_ptr<IXRHIBufferUpload> UploadBufferAsync(const IXRHIBufferDesc& desc,
                                                                 const void* src,
                                                                 std::size_t byteCount) = 0;

    // Format fallback queries (e.g. sRGB sampled support) without touching
    // Vulkan from renderer code. Needed by texture upload paths.
    virtual bool IsTextureFormatSupported(IXRHIFormat format, IXRHITextureUsage usage) const = 0;

    virtual std::unique_ptr<IXRHIFence> CreateFence(bool signaled) = 0;
    virtual std::unique_ptr<IXRHISemaphore> CreateSemaphore() = 0;

    // Render-target owning its pass + framebuffer in the backend.
    virtual std::unique_ptr<IXRHIRenderTarget> CreateRenderTarget(
        const IXRHIRenderTargetDesc& desc) = 0;

    // Host-side GPU drain for teardown/recreation paths (offscreen Recreate).
    // Must not be called from inside a recording command list.
    virtual void WaitIdle() = 0;

    virtual const IXRHICapabilities& GetCapabilities() const = 0;
};

} // namespace ixrhi
