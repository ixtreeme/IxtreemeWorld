# Phase-2/3 migration status (updated at Phase-3F completion)

IXRHI owns the graphics frame contract; IXVulkan implements it. The legacy
VulkanDevice keeps device/queue/swapchain-handle infrastructure plus synced
migration shims; its own frame loop is dormant (classification: C, with the
remaining infrastructure migration tracked as debt below).

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
- `SkinnedMeshRenderer.*` (Phase 3D) — full compute + graphics migration,
  zero `Vk*` (verified by grep): rest-vertex SSBO, per-frame/per-slot bone
  palettes (host-visible) + skinned-output buffers (Storage|Vertex|
  TransferSrc), index + 64 host-visible UBOs, 2 IXRHI textures + samplers,
  1 graphics bind-group layout + 128-slot group (UBO + sampler), 1 compute
  bind-group layout + 64-slot group (rest/palette/output storage buffers),
  compute pipeline (`skinned_mesh_cs.spv`, CSMain, push constants,
  64-thread groups) + opaque graphics pipeline + front-cull reflection
  pipeline, compute dispatches + compute→graphics barriers
  (`TransitionBuffer` ShaderWrite→VertexRead) recorded into the same
  graphics command list pre-pass (no async compute — parity), draws through
  `IXRHICommandList`, Ozz stays behind the renderer boundary (no Ozz types
  in IXRHI). CPU skinning loop kept for load-time bounds + one-time
  GPU-vs-CPU verification (`ExecuteAndWait` + readback, then barriers only).
- `CubeRenderer.*` DELETED (Phase 3A §44: proven dead, zero references).
  `shaders/Cube.hlsl` + cube SPIR-V build rules intentionally retained
  (inert data; removal is unrelated churn).
- `RmlUiLayer.*` (Phase 3E) — full UI migration, zero `Vk*` (verified by
  grep): RmlUi 6.2 callback set preserved (Compile/Render/ReleaseGeometry,
  Load-stub/Generate/ReleaseTexture, scissor region, SetTransform no-op),
  per-geometry host-visible VB/IB, premultiplied-alpha RGBA8 generated
  textures (font atlas + runtime images) via backend-staged upload, one
  shared linear/clamp sampler, 1 bind-group layout (separate SampledImage +
  Sampler bindings, mirroring the shader's apart-declared g_texture /
  g_sampler) + 256-slot group with free-list slot reuse, 1 UI pipeline
  (premultiplied ONE/ONE_MINUS_SRC_ALPHA blend, depth off, push-constant
  viewport+translation), deferred retire-frame (+3) geometry/texture release
  with slot reclaim, recording through the frame-owned `IXRHICommandList`.
  New minimal primitives: `SampledImage`/`Sampler` binding types +
  `UpdateSampledImage`/`UpdateSampler` (D3D12: SRV / sampler-heap entries).
- `TerrainRenderer.*` (Phase 3F) — full terrain + water + shadow + reflection
  migration, zero `Vk*` (verified by grep): host-visible mesh/uniform VB/IB,
  CPU-generated mipmapped RGBA8/R8 arrays + 2D textures via staged upload,
  full-texture splat rewrite via `UpdateTexture`, Read-modify-Write sculpt
  spans (same GPU bytes as mapped writes), 1 terrain bind layout (UBO + 9
  combined) with 4 (view, frame) slots + 1 water layout (UBO + 6 combined)
  with per-(body, view, frame) slots, 4 pipelines (main alpha-blend LEQUAL,
  front-cull reflection, water depth-test-only, VS-only shadow with bias),
  depth-only per-cascade shadow targets over a D32 4-layer array with
  comparison sampler + explicit attachment→read transition, swapchain-format
  reflection target with real pass token forwarded to skinned guests.
  Deleted as provably dead (no callers): per-layer DDS diffuse/mask GPU path
  (sole BC-format consumer), tile/dominant-texture helpers, `DrawTerrainSurface`,
  vestigial shared water sets, `m_layerDescriptorSets`. New minimal
  primitives: `R8Unorm`/`R32Float` formats, sampler `mipLodBias` +
  `compareEnable/compareOp` + `ClampToBorder`, array/mip creation upload,
  `UpdateTexture`, depth-only + `depthLayer` render targets, optional
  fragment shader + depth bias.

## Still native (inventoried, unchanged behavior)

- Legacy `VulkanDevice` infrastructure: instance, debug messenger, surface
  handle, physical/logical device, queues, swapchain handle + images + views +
  depth, formats/extents, validation, FindMemoryType. Its frame loop
  (Begin/End/BeginSwapchainRenderPass/timestamps/Resize) is dormant. All
  frame-migration shims deleted in Phase 3F (see below) — no generic native
  renderer remains.
- `NativeWindow` surface API retired (Phase 3C): `DescribeNative()` only;
  surface creation lives in backend `IXVulkanSurface`.

## Native escape hatches (complete inventory)

- E1 `SetPipelineRenderPass` — REMOVED in Phase 3A.
- E2 `Loop()` — RETIRED as frame authority in Phase 3C. The `Loop()` accessor
  remains as backend-internal infrastructure access only (device/queues/
  swapchain); no generic code drives the loop through it.
- E3 `WrapFrameCommandList` — DELETED in Phase 3C.
- E4 frame-migration shims (`SetMigrationFrameState`, `SetMigrationMainPass`,
  `GetCommandBuffer`, `GetFrameIndex`, `IsFrameActive`, `GetSafeFrameNumber`,
  `GetFrameNumber`, legacy `GetRenderPass` mirror) — DELETED in Phase 3F.
  Generic legacy frame consumer count = 0 (Terrain was the last). Backend
  `ResolveRenderPass(null)` now uses its own swapchain pass; the vestigial
  `m_swapchainDirty` legacy flag is gone too.
- Bridge-scoped native resolution (`NativeViewOf`/`NativeSamplerOf`/
  `NativePassOf` in `IXVulkanBridge.h`): backend-private, for the editor
  adapter's UI registration (no renderer uses it anymore).

## Phase-3C contract additions (all backend-implemented, smoke-tested)

- `IXRHIFrame.h`: `IXRHIFrameResult` (Success/Skip/SwapchainRecreated/
  DeviceLost), canonical `IXRHIFrameInfo` (frame/image indices, borrowed
  command list + backbuffer, generation, frame number), `IXRHIFrame`
  (result + info + Debug pairing token). `IXRHIFrameInfo` consolidation
  replaces the Phase-2 snapshot struct — one representation, not two.
- `IXRHIDevice` frame lifecycle: `BeginFrame`/`EndFrame`/`RequestResize`/
  `GetFramesInFlight`/`GetSwapchainGeneration`/`GetMainSwapchain`/
  `GetMainRenderTarget`/`GetMainPass`, plus `Shutdown` with documented order.
- `IXRHIQuery.h`: `IXRHITimestampPoint` (legacy 23-point ordering 1:1),
  nanosecond results, CPU frame timing; capture-on-request parity.
- `IXRHISwapchain`: generation counter; acquire/present stay in the device
  lifecycle (single flow), backbuffer via the frame context.
- `IXVulkanFrameTracker`: pure state machine + generation (unit-tested).
- `IXVulkanSwapchain`: main pass + per-image targets/backbuffers/depth.
- `IXVulkanSurface` (header-only): backend-owned Win32/Android creation.
- `NativeWindowDesc`: windows/surface handles without Vulkan includes.
- `TranslateFrameResult` (pure, unit-tested): VkResult → frame result,
  unknown → DeviceLost, never silent success.

## Legacy VulkanDevice future (classification: C)

Still contains backend infrastructure that must migrate later: instance,
physical/logical device, queues, swapchain handle + images + views + depth,
surface handle, formats, validation, FindMemoryType. Dormant: frame loop,
frame sync objects, timestamp pool, Resize, render-pass/framebuffer creation.
Phase-3F deleted all frame shims (`SetMigrationFrameState`,
`SetMigrationMainPass`, `GetCommandBuffer`, `GetFrameIndex`, `IsFrameActive`,
`GetSafeFrameNumber`, `GetFrameNumber`, legacy `GetRenderPass` mirror, legacy
`m_swapchainDirty`); `GetSwapchainImage/View` + `GetPresentQueue` remain as
backend-internal swapchain plumbing. Long-term model: `IXVulkanDevice →
Vulkan API` (§78 `IXVulkanContext` or equivalent absorbs the remainder).

## Phase-3F contract additions (all backend-implemented, smoke-tested)

- `IXRHIFormat::R8Unorm` + `R32Float` (terrain splat/mask arrays, water edge
  alpha) with conversions + byte sizes.
- `IXRHISamplerDesc`: `mipLodBias`, `compareEnable` + `compareOp`
  (shadow comparison sampling), `ClampToBorder` address mode (opaque-white
  border convention).
- `IXRHITexture`: `MipLevels()` + `ArrayLayers()`; creation upload covers
  mipmapped arrays in documented tight layer-major packing;
  `IXRHIDevice::UpdateTexture` for synchronous full base-level rewrites.
- `IXRHIRenderTargetDesc`: depth-only targets (null color) + `depthLayer`
  for layered depth (shadow cascades).
- `IXRHIGraphicsPipelineDesc`: optional fragment shader (depth-only) +
  `IXRHIDepthBias` (constant/slope).
- Backend correctness fixes found by migration: depth-aspect barrier ranges
  cover full mips/layers; depth sampled as texture uses the depth-read
  layout in barriers and descriptor writes (also fixes the offscreen depth
  snapshot path).

## Recommended follow-ups (not 3F scope)

- Legacy `VulkanDevice` absorption into the backend (`IXVulkanContext` or
  equivalent): instance/device/queues/swapchain-handle migration.
- Fence-based retirement queue replacing blanket shared ownership.
- `WaitIdle` in offscreen resize → affected-frame wait or deferred retire.
- Workstation validation: terrain/water/shadow/reflection visual parity,
  edit stress (§89), scene reload (§90), resize (§92) — build + contract
  verified, GPU runtime unverified (§100).
