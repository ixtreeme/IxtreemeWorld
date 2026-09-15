# Phase-2 migration status (updated at Phase-2 completion)

Strangler state: migrated and native renderers coexist. The frame loop,
swapchain pacing, validation layers and GPU timestamps still live in
`libs/platform/VulkanDevice`; the IXRHI backend borrows them.

## IXRHI-native (zero Vk*, verified by grep)

- `libs/render/SelectionOutlineRenderer.*` — buffers, bind group, pipeline,
  in-pass recording. Targets the offscreen pass via the borrowed
  `IXRHIRenderPass` token (`SetTargetPass`, Phase 3A; the E1 global override
  is deleted).
- `libs/render/WorldLabelRenderer.*` — buffers, font-atlas texture upload,
  sampler, bind group, pipeline. Swapchain pass via backend default.
- `libs/render/StaticMeshRenderer.*` (Phase 3A) — device-local VB/IB with
  staged upload, 128 host-visible UBOs, 2 growable storage instance buffers
  (+CPU mirror, no readback), 3 baked + N cached material textures with
  samplers, 1 bind-group layout + 128-slot group (UBO + 3 samplers + SSBO),
  5 pipeline variants (opaque/mask/unlit/unlitMask/outline), instanced
  two-pass (opaque then mask) + outline draws through `IXRHICommandList`,
  async LOD index uploads via `IXRHIBufferUpload`. No shadow/wireframe/
  push-constant paths existed to migrate (verified absent).
- `CubeRenderer.*` DELETED (Phase 3A §44: proven dead, zero references).
  `shaders/Cube.hlsl` + cube SPIR-V build rules intentionally retained
  (inert data; removal is unrelated churn).

## Still native (inventoried, unchanged behavior)

- `VulkanDevice` (device/swapchain/pass/commands/sync/timestamps/validation).
- `OffscreenSceneRenderer` — exposes `VkRenderPass/ImageView/Sampler/Extent`
  (consumed by editor views, terrain refraction inputs, and the Phase-3A
  borrowed-pass token). Migration = Phase 3B.
- `SkinnedMeshRenderer` (+compute skinning + push constants), `TerrainRenderer`
  (+water/shadow), `RmlUiLayer`, `EditorImGui` Vulkan backend + scene/game view
  triplets.
- `NativeWindow::CreateVulkanSurface` leak: unchanged, still the only surface
  path. Dies with the frame-loop migration (backend creates the surface from a
  generic native-handle descriptor).

## Native escape hatches (complete inventory)

- E1 `IXVulkanDevice::SetPipelineRenderPass` — REMOVED in Phase 3A (replaced by
  the borrowed `ixrhi::IXRHIRenderPass` token in pipeline descs).
- E2 `IXVulkanDevice::Loop()` (borrowed frame-loop owner).
- E3 `ixvulkan::WrapFrameCommandList` (borrowed recording command buffer).
- ImGui/RmlUi backends: untouched native code, no NEW hatch (they don't go
  through IXRHI yet — editor graphics bridge is Phase 3B).

## Phase-3A contract additions (all backend-implemented, smoke-tested)

- `IXRHIRenderPass` token + `IXRHIGraphicsPipelineDesc::targetRenderPass`.
- `IXRHIBufferUpload` + `IXRHIDevice::UploadBufferAsync` (non-blocking staged
  upload; LOD path is the first consumer).
- `IXRHIDevice::IsTextureFormatSupported` (sRGB fallback without Vulkan).
- `IXRHIFormat::R8G8B8A8Srgb` (base-color color space parity).
- `IXRHICommandList::SetIndexBuffer` (+ `IXRHIFrameInfo::frameNumber` for
  diagnostics).

## Next (recommended Phase 3B)

OffscreenSceneRenderer + editor Vulkan decoupling (Phase 3A proves IXRHI
stable on the production mesh path):

- generic offscreen outputs (`IXRHITexture` color/depth instead of
  `VkImageView`/`VkSampler`/`VkRenderPass` exposure),
- backend-created render passes from attachment descs (retire the borrowed
  token's last native source),
- `EditorImGui` scene/game view triplets through an editor graphics bridge,
- `NativeWindow` surface descriptor (remove `CreateVulkanSurface` leak),
- frame-loop migration (E2/E3 retire): `BeginFrame/Acquire/Submit/Present`
  on `IXRHIDevice`, timestamps via `IXRHIQueryPool`.

SkinnedMesh + compute (3C) and Terrain/Water prep follow 3B.
