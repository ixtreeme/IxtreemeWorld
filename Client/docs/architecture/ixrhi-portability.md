# IXRHI portability baseline (Phase 2, updated Phase 3C, compute review Phase 3D, UI review Phase 3E)

No MoltenVK integration in this phase. This document records what the core
guarantees, what is optional, and which Vulkan-specific assumptions remain.

## Core required features (MoltenVK-capable by design)

- 2D sampled + attachment textures (RGBA8/BGRA8, D32F/D24S8 depth), 1 sample.
- Vertex/index/uniform/storage buffers, host-visible coherent uploads.
- Graphics pipelines: line/triangle lists, fill, cull none/front/back,
  depth test/write + LEQUAL/LESS, single-attachment alpha blend.
- Combined image samplers (linear, clamp-to-edge). Anisotropy optional.
- SPIR-V vertex/fragment/compute shaders (DXC output) — the same blobs
  MoltenVK consumes later. Entry points are explicit strings (VSMain/PSMain).
- Dynamic viewport/scissor. Indirect draw + multi-draw-indirect: optional.
- Fences + binary semaphores only (no timeline semaphores in the contract).

## Optional capabilities (queried via IXRHICapabilities, never assumed)

compute, indirectDraw, multiDrawIndirect, timestampQueries, anisotropy,
dynamicRendering (false on the current VkRenderPass backend), bindless,
rayQuery/rayTracing, meshShaders, variableRateShading, asyncCompute.

## Known Vulkan-specific assumptions still in the tree

1. `VkRenderPass` framebuffers: the backend resolves pipelines against a
   borrowed pass (override or swapchain pass). Dynamic rendering is the
   portability exit; descs already carry color/depth formats for it.
2. `vkQueueWaitIdle` setup uploads: fine on MoltenVK, replaceable with
   fence-waits without API change.
3. GLSL-style `VSMain/PSMain` entry points: engine convention, backend-agnostic.
4. `VK_IMAGE_LAYOUT_*` sequencing hidden in the backend (upload barriers);
   render-pass attachment layouts stay implicit until the frame-loop migration.
5. Validation layers (`VK_LAYER_KHRONOS_validation`): desktop-only; absence is
   tolerated (logged, non-fatal) so the same binary path works where layers
   don't exist.

## Phase-3C frame/swapchain review (MoltenVK-relevant)

- Surface creation: `NativeWindowDesc` (Win32 HWND/HINSTANCE, Android
  `ANativeWindow*`) → backend `CreateSurfaceForWindow`. Apple adds a
  `VK_EXT_metal_surface` branch + descriptor type here; no IXRHI change.
- Present: binary-semaphore acquire/present, FIFO-or-uncapped preserved from
  surface capabilities. No timeline semaphores, no present-wait extensions.
- Swapchain formats/extents: re-queried from the surface on every recreate;
  generation counter isolates stale references (same discipline ports).
- Frames in flight = 2, single universal queue. No async-compute assumption.
- Timestamp queries: gated on `timestampComputeAndGraphics`; absent on some
  MoltenVK configurations → backend no-ops, profiler reports unavailable
  (same as the legacy verbose-diagnostics gating).
- Zero-size handling: Skip without spinning; recreation deferred until a
  non-zero size arrives (matches mobile backgrounding needs).
- `VK_SUBOPTIMAL_KHR` continues with warn-once (same as before); only
  `OUT_OF_DATE` forces recreation. No platform-specific swapchain flags.

## Future MoltenVK concerns (not implemented)

- Portability-subset validation (`VK_KHR_portability_subset`) will need a few
  desc clamps (e.g. sampler LOD, multisample counts) — centralize in
  IXVulkanConversions when that backend lands.
- No Metal backend, no separate shader language: SPIR-V stays the interchange.

## Phase-3E UI review (MoltenVK-relevant, no Apple code)

- Blend ONE/ONE_MINUS_SRC_ALPHA on color+alpha, depth off, scissor-only
  clipping, R8G8B8A8_UNORM sampled textures, linear clamp-to-edge sampling
  (single shared sampler, no anisotropy, maxLod 1, no mipmaps) — all inside
  the portability subset; no separate-sampler friction (Metal supports
  argument-buffer or bound texture+sampler pairs behind the backend).
- Dynamic UI buffers are host-visible coherent uploads (no new memory-model
  assumption); texture staging + TransferDst→ShaderRead transitions stay
  backend-internal, with no per-draw queue stall.
- Vertex input R32G32 + R8G8B8A8_UNORM + R32G32 at 20-byte stride: portable
  attribute formats; compile-time layout asserts guard the RmlUi 6.2 struct.

## Phase-3D compute review (MoltenVK-relevant, no Apple code)

- Workload: 3 storage buffers (read-only rest vertices, read-only bone
  palette, read-write skinned output), one `numthreads(64,1,1)` dispatch per
  (frame, skin slot), same-queue compute→graphics barrier
  (ShaderWrite→VertexRead). No atomics, no groupshared memory, no
  cross-queue semaphore — all inside the portability subset.
- Shader interchange stays SPIR-V via DXC (`CSMain`); the HLSL uses
  `vk::binding` + `register()` annotations that DXC lowers the same way for
  the graphics stages already in use — no new shader-language assumption.
- `Storage` buffer usage + host-visible coherent palette upload uses the
  same coherent-upload path as the existing uniform/storage usage; no new
  memory-model assumption.
- Frames in flight = 2 discipline unchanged: per-frame/per-slot palette +
  output buffers (64 slots each), no buffer sharing across in-flight frames.
