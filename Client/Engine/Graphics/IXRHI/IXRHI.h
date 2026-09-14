#pragma once

// IXRHI — Ixtreeme Render Hardware Interface (Phase 1: boundary reservation only).
//
// Phase 1 rule: this header reserves the canonical IXRHI naming and handle model.
// It intentionally contains NO backend implementation and NOTHING includes it yet
// from the Vulkan renderer. The live renderer (libs/platform/VulkanDevice.h and
// libs/render/*Renderer.*) keeps working unchanged. Phase 2 (IXRHI migration) will
// make renderers depend on these types instead of Vk* types.
//
// Canonical naming (do NOT use RHI / RHIDevice / VulkanRHI):
//   IXRHI*    — platform-independent interface types
//   IXVulkan* — Vulkan backend implementation types (Engine/Graphics/Vulkan/)
//
// Dependency direction (future):
//   StaticMeshRenderer / SkinnedMeshRenderer / TerrainRenderer / WaterRenderer /
//   SceneRenderer / SelectionOutline / WorldLabel / Offscreen
//       -> IXRHI -> IXVulkan -> native Vulkan (Win/Linux) or MoltenVK->Metal (Apple)

#include <cstdint>

namespace ixrhi
{

using IXRHIBool = bool;

// Opaque handle types. Phase 2 will back these with real Vulkan objects owned by
// IXVulkan*. For now they are intentionally empty structs so no Vk* type leaks
// through this boundary.
struct IXRHIBufferHandle
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

struct IXRHITextureHandle
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

struct IXRHIShaderHandle
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

struct IXRHIPipelineHandle
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

struct IXRHISamplerHandle
{
    std::uint64_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

// Capability probe returned by the backend. Renderers must branch on this instead
// of #ifdef _WIN32 / __ANDROID__ once Phase 2 lands.
struct IXRHICapabilities
{
    bool discreteGpu = false;
    bool supportsBindless = false;
    bool supportsRayQuery = false;
    std::uint32_t maxAnisotropy = 1;
};

} // namespace ixrhi
