#pragma once

// IXRHIRenderTarget — engine-level render-pass target (Phase 3B, §23).
//
// Minimal attachment concept, NOT a RenderGraph: one color target plus an
// optional depth target, load/store ops, and clear values. The backend owns the
// VkRenderPass + VkFramebuffer internally (no dynamic rendering in tree yet);
// generic code sees only this object plus its borrowed IXRHIRenderPass (for
// baking pipelines against the target, e.g. static meshes into the scene
// target). Load vs clear variants are separate targets sharing textures,
// mirroring the old clear/load pass pair.

#include "IXRHI.h"
#include "IXRHIRenderPass.h"
#include "IXRHITypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ixrhi
{

class IXRHITexture;
class IXRHICommandList;

struct IXRHIRenderTargetDesc
{
    std::shared_ptr<IXRHITexture> color;
    std::shared_ptr<IXRHITexture> depth; // null = no depth attachment
    IXRHILoadOp colorLoad = IXRHILoadOp::Clear;
    IXRHIStoreOp colorStore = IXRHIStoreOp::Store;
    IXRHILoadOp depthLoad = IXRHILoadOp::Clear;
    IXRHIStoreOp depthStore = IXRHIStoreOp::Store;
    float clearColor[4] = {0.04f, 0.05f, 0.09f, 1.0f};
    float clearDepth = 1.0f;
    std::uint32_t clearStencil = 0;
    std::string debugName;
};

class IXRHIRenderTarget
{
public:
    virtual ~IXRHIRenderTarget() = default;

    // Begins the target's pass on cmd and sets full-target viewport/scissor.
    // Const: recording touches the command buffer, not target state.
    virtual void Begin(IXRHICommandList& cmd) const = 0;
    virtual void End(IXRHICommandList& cmd) const = 0;

    // Borrowed pass for pipeline baking. Lifetime = this target's lifetime,
    // which the target owner (offscreen renderer) keeps longer than any
    // pipeline baked against it.
    virtual const IXRHIRenderPass* GetPass() const = 0;

    virtual std::uint32_t Width() const = 0;
    virtual std::uint32_t Height() const = 0;
};

} // namespace ixrhi
