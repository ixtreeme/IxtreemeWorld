# Phase-2/3 migration status (updated at Phase-3B completion)

Strangler state: migrated and native renderers coexist. The frame loop,
swapchain pacing, validation layers and GPU timestamps still live in
`libs/platform/VulkanDevice`; the IXRHI backend borrows them.

## IXRHI-native (zero Vk*, verified by grep)

- `libs/render/SelectionOutlineRenderer.*` — buffers, bind group, pipeline,
  in-pass recording. Targets the offscreen pass via the borrowed
  `IXRHIRenderPass` token (`SetTargetPass`).
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
- `libs/render/OffscreenSceneRenderer.*` (Phase 3B) — IXRHI color/depth/
  snapshot textures + sampler, backend-owned clear/load render targets
  (passes + framebuffers), IXRHI composite pipeline (fullscreen triangle +
  Reinhard tone map), snapshot copies + layout transitions through
  `IXRHICommandList`. Exposes `GetTargetPass()` (borrowed token),
  `GetColorTexture()`, snapshot textures, sampler, dimensions.
- `libs/render/EditorImGui.*` + panels (Phase 3B) — zero Vulkan types, no
  Vulkan include; scene/game views + asset-preview thumbnails resolve opaque
  UI ids through `IEditorTextureProvider`. ImGui backend + UI registrations
  isolated in `IXVulkanEditorAdapter` (`Engine/Graphics/Vulkan`).
- Terrain refraction seam (Phase 3B) — inputs are IXRHI-owned (`shared_ptr`
  texture/sampler + dimensions); native views resolve backend-locally at
  descriptor-write time. Terrain/Water rendering itself stays native.
- `SkinnedMeshRenderer` / `TerrainRenderer::SetTargetPass` (Phase 3B) — take
  the borrowed `IXRHIRenderPass` token, unwrapped backend-locally. No caller
  passes `VkRenderPass` anymore.
- `CubeRenderer.*` DELETED (Phase 3A §44: proven dead, zero references).
  `shaders/Cube.hlsl` + cube SPIR-V build rules intentionally retained
  (inert data; removal is unrelated churn).

## Still native (inventoried, unchanged behavior)

- `VulkanDevice` (device/swapchain/pass/commands/sync/timestamps/validation).
- `SkinnedMeshRenderer` (+compute skinning + push constants),
  `TerrainRenderer` (+water/shadow/reflection), `RmlUiLayer` (no offscreen
  coupling — verified, migration is a later phase).
- `NativeWindow::CreateVulkanSurface` leak: unchanged, still the only surface
  path. Dies with the frame-loop migration (backend creates the surface from a
  generic native-handle descriptor).

## Native escape hatches (complete inventory)

- E1 `IXVulkanDevice::SetPipelineRenderPass` — REMOVED in Phase 3A (replaced by
  the borrowed `ixrhi::IXRHIRenderPass` token in pipeline descs).
- E2 `IXVulkanDevice::Loop()` (borrowed frame-loop owner).
- E3 `ixvulkan::WrapFrameCommandList` (borrowed recording command buffer).
- Bridge-scoped native resolution (`NativeViewOf`/`NativeSamplerOf`/
  `NativePassOf` in `IXVulkanBridge.h`): backend-private, for in-transition
  native consumers (terrain refraction, skinned/terrain pass tokens) and the
  editor adapter's UI registration. Counted under the existing bridge — the
  E-count is unchanged (net −1 after E1's removal).

## Phase-3B contract additions (all backend-implemented, smoke-tested)

- `IXRHIRenderTarget` + `IXRHIDevice::CreateRenderTarget` (backend-owned
  pass + framebuffer from IXRHI textures; clear/load variants).
- `IXRHICommandList::TransitionTexture` / `CopyTexture` (explicit layout
  tracking by the caller; uniform stage/access mapping in backend).
- `IXRHIImageLayout::TransferSrc`.
- `IXRHIDevice::WaitIdle` (teardown/recreation paths).
- `IXRHIRenderPass` now also produced by render targets (`GetPass()`); the
  app-side `Borrow` of the native pass is deleted.
- `Editor/Graphics` lib (`IXEngineEditorGraphics`): `EditorGraphicsBridge`
  registry + `IEditorTextureProvider` (`EditorTextureHandle`, scene/game
  slots, preview pool).
- `IXVulkanEditorAdapter` (in `IXEngineVulkan`, editor-only): ImGui backend
  lifecycle, backend NewFrame/draw submission, UI texture registration.

## Next (recommended Phase 3C)

IXRHI Frame Lifecycle + Swapchain + Command Ownership:

- `IXRHIDevice`: `BeginFrame`/`Acquire`/`Submit`/`Present`/`EndFrame`,
  retiring E2 `Loop()` and E3 `WrapFrameCommandList`.
- `IXRHIQueryPool` for GPU timestamps (migrate `VulkanDevice` capture flow).
- Platform surface behind `IXVulkan` (retire `CreateVulkanSurface` leak).
- SkinnedMesh + compute migration on the owned command lists; Terrain/Water
  prep follows.
