# Phase-2 migration status (updated at Phase-2 completion)

Strangler state: migrated and native renderers coexist. The frame loop,
swapchain pacing, validation layers and GPU timestamps still live in
`libs/platform/VulkanDevice`; the IXRHI backend borrows them.

## IXRHI-native (zero Vk*, verified by grep)

- `libs/render/SelectionOutlineRenderer.*` — buffers, bind group, pipeline,
  in-pass recording. Pipeline pass resolves via the backend override set by the
  frame owner (offscreen scene pass).
- `libs/render/WorldLabelRenderer.*` — buffers, font-atlas texture upload,
  sampler, bind group, pipeline. Swapchain pass via backend default.

## Still native (inventoried, unchanged behavior)

- `VulkanDevice` (device/swapchain/pass/commands/sync/timestamps/validation).
- `OffscreenSceneRenderer` — exposes `VkRenderPass/ImageView/Sampler/Extent`
  (consumed by editor views, terrain refraction inputs, and the Phase-2 pass
  override). Migration = Phase 3B.
- `StaticMeshRenderer` (+5 pipeline variants), `SkinnedMeshRenderer`
  (+compute skinning + push constants), `TerrainRenderer` (+water/shadow),
  `CubeRenderer` (dead code — migrate or delete in Phase 3), `RmlUiLayer`,
  `EditorImGui` Vulkan backend + scene/game view triplets.
- `NativeWindow::CreateVulkanSurface` leak: unchanged, still the only surface
  path. Dies with the frame-loop migration (backend creates the surface from a
  generic native-handle descriptor).

## Native escape hatches (complete inventory)

- E1 `IXVulkanDevice::SetPipelineRenderPass` (borrowed pass override).
- E2 `IXVulkanDevice::Loop()` (borrowed frame-loop owner).
- E3 `ixvulkan::WrapFrameCommandList` (borrowed recording command buffer).
- ImGui/RmlUi backends: untouched native code, no NEW hatch (they don't go
  through IXRHI yet — editor graphics bridge is Phase 3B).

## Next (recommended Phase 3A first)

StaticMeshRenderer full migration: needs index-buffer + multi-set bind-group
support in the backend (both already exist: `DrawIndexed` + `BindGroup` are
implemented and smoke-adjacent). Then offscreen/editor decoupling (3B), then
skinned+compute (3C), terrain/water last.
