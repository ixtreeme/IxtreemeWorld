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
#include "IXRHIFrame.h"
#include "IXRHIPipeline.h"
#include "IXRHIQuery.h"
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

    // ---- Frame lifecycle (Phase 3C: IXRHI owns the graphics frame contract).
    // BeginFrame waits/recycles the frame slot, acquires the swapchain image,
    // resets per-frame command resources and begins recording. Returns Success
    // with a valid frame context, or Skip/SwapchainRecreated/DeviceLost (in
    // which cases EndFrame must NOT be called).
    virtual IXRHIFrame BeginFrame() = 0;
    // Ends recording, submits, presents, advances the frame slot. The frame
    // must be a successful BeginFrame result from this device (Debug-checked).
    virtual void EndFrame(const IXRHIFrame& frame) = 0;
    // Queues a resize; the actual recreation happens in BeginFrame (which then
    // reports SwapchainRecreated). Returns true when the size differs.
    virtual bool RequestResize(std::uint32_t width, std::uint32_t height) = 0;
    virtual std::uint32_t GetFramesInFlight() const = 0;
    virtual std::uint64_t GetSwapchainGeneration() const = 0;
    virtual IXRHISwapchain& GetMainSwapchain() = 0;
    // Main-window render target over the current backbuffer (valid during a
    // successful frame only; framebuffer tracks the acquired image).
    virtual IXRHIRenderTarget* GetMainRenderTarget() = 0;
    // Main-window pass for pipeline baking (recreated with the swapchain).
    virtual const IXRHIRenderPass* GetMainPass() const = 0;

    // ---- GPU timestamps (capture-on-request, same semantics as before).
    virtual void WriteTimestamp(IXRHITimestampPoint point) = 0;
    virtual void WriteTimestamp(std::uint32_t pointIndex) = 0;
    virtual void RequestGpuFrameCapture() = 0;
    virtual bool TryReadTimestamps(IXRHITimestampResults& gpu,
                                   IXRHICpuFrameTiming& cpu) = 0;

    virtual const IXRHICapabilities& GetCapabilities() const = 0;
};

} // namespace ixrhi
