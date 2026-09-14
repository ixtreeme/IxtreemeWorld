# Phase 2 (IXRHI) — exact recommended scope

DO NOT start before Phase 1 is green (configure + full build + smoke tests below).

## 1. Relocate VulkanDevice behind IXVulkanDevice

- Move `libs/platform/VulkanDevice.h/.cpp` -> `Engine/Graphics/Vulkan/` as `IXVulkanDevice`
  (keep a forwarding header + `using VulkanDevice = ixvulkan::IXVulkanDevice` until call sites migrate).
- `NativeWindow::CreateVulkanSurface` stays platform-level but returns an `IXRHI` surface handle,
  not `VkSurfaceKHR`.

## 2. Migrate renderers class-by-class (one per commit, behavior-gated screenshots)

Order (lowest coupling first): `CubeRenderer` -> `WorldLabelRenderer` ->
`SelectionOutlineRenderer` -> `OffscreenSceneRenderer` (defines the shared pass/snapshot API) ->
`StaticMeshRenderer` (+ `MeshSystem`/`LODSystem`) -> `SkinnedMeshRenderer`
(CS skinning kernel = first `IXRHIComputePipelineDesc` consumer) -> `TerrainRenderer` + water ->
`EditorImGui` Vulkan backend (last — it owns the descriptor pool + scene/game view triplets).

Per-class checklist: DEVICE/LIFETIME, BUFFERS (`IXRHIBufferDesc/Handle`),
TEXTURES (`IXRHITextureDesc/Handle`), SHADERS (`IXRHIShaderDesc/Handle`),
PIPELINES (`IXRHIGraphicsPipelineDesc` / `IXRHIComputePipelineDesc`),
DESCRIPTORS, COMMANDS (`IXRHICommandList`), SYNCHRONIZATION
(`IXRHIFence`/`IXRHISemaphore`), SWAPCHAIN (`IXRHISwapchain`), RENDER PASSES, QUERIES
(`IXRHIQuery`), PLATFORM SURFACE.

## 3. Cut renderer<-editor coupling identified in Phase 1

- Replace `SetSceneViewTexture(VkSampler, VkImageView, ...)` with an `IXRHITextureHandle` view struct.
- `OffscreenSceneRenderer` getters return `IXRHI*` handles; editor views consume them opaquely.

## 4. Split libs/render (transitional) into Engine/Graphics/Renderer + Editor/*

- Engine side: all `*Renderer.*` + `SpatialIndex` (local-only) + `WorldCamera`.
- Editor side: `EditorImGui.*`, `editor_panels/*.inl`, `tools/tree/*`, `UIHelpers.*`.
- `RuntimeSession`/`RuntimeUiAdapter`/`SceneManager` move per their headers' notes
  (RuntimeSession -> `Engine/Runtime/`; scene serialization stays engine but presentation moves out).

## 5. Non-goals (still forbidden in Phase 2)

RenderGraph, BGFX, Metal backend, MMORPG networking, Auriga gameplay, 100km streaming,
serialization rewrite. MoltenVK mapping is design-only until the Vulkan side is handle-clean.
