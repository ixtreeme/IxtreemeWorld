# Vulkan leaks outside Graphics/Vulkan (Phase 1 inventory — for Phase 2 IXRHI migration)

Rule for Phase 2: NO `Vk*` type may escape `Engine/Graphics/Vulkan`. Phase 1 only
documents + structurally isolates (new code must use `ixrhi::IXRHI*` handles).

## Leak sites (audited, behavior unchanged)

DEVICE/LIFETIME
- `libs/platform/VulkanDevice.h/.cpp` — VkInstance/Device/PhysicalDevice/Queue/Swapchain/
  RenderPass/CommandPool/QueryPool/Semaphore/Fence + SwapchainSupport/QueueFamilies helpers.
  (Future home: `IXVulkanDevice` behind `Engine/Graphics/Vulkan/`.)
- `libs/platform/NativeWindow.h`, `NativeWindow_Win32.h`, `NativeWindow_Android.h` —
  `CreateVulkanSurface(VkInstance, VkSurfaceKHR*)`.

SWAPCHAIN / RENDER PASSES / SYNCHRONIZATION
- `VulkanDevice` owns swapchain images/views/framebuffers, depth-stencil images, main render
  pass, per-frame command buffers, imageAvailable/renderFinished/inFlightFences, timestamp pool.

BUFFERS / TEXTURES / DESCRIPTORS
- `libs/render/StaticMeshRenderer.*`, `SkinnedMeshRenderer.*` (vertex/index/uniform/storage buffers,
  descriptor sets, skinning CS pipeline), `TerrainRenderer.*` (heightmap/splat textures, chunk buffers),
  water paths inside terrain/water IO, `WorldLabelRenderer.*`, `OffscreenSceneRenderer.*`
  (color/depth images + snapshot views), `CubeRenderer.*`.

SHADERS / PIPELINES
- All of the above create VkShaderModule/VkPipeline/VkPipelineLayout from DXC SPIR-V in
  `assets/shaders/*.spv` (built by `IXEngineShaders` from `shaders/*.hlsl`).

COMMANDS
- Per-frame `vkBeginCommandBuffer`/`vkCmd*` recording inside each renderer + `EditorImGui::Render`.

PLATFORM SURFACE
- Win32 `VK_USE_PLATFORM_WIN32_KHR` in `EditorImGui.h`; Android surface in `NativeWindow_Android`.

EDITOR-STATE COUPLING (renderer depends on editor — must be cut in Phase 2)
- `EditorImGui.h` holds VkDevice/PhysicalDevice/Queue/DescriptorPool + scene/game view
  VkSampler/ImageView/DescriptorSet triplets; `SetSceneViewTexture`/`SetGameViewTexture`
  take raw `VkSampler/VkImageView/VkImageLayout/VkExtent2D`.
- `OffscreenSceneRenderer.h` exposes `VkRenderPass/VkImageView/VkSampler/VkExtent2D` getters
  consumed by editor scene/game views + water refraction.

## Include-policy note

`IXEngineRender` currently exports `${CLIENT_LIBS_DIR}` as PUBLIC include root, which is what
lets `apps/client` `#include "platform/process.h"`-style short paths work. New boundaries
(`Editor/Build`, `Engine/*`) add their own directory to their target's PUBLIC includes and
include their own headers by basename. Do NOT add umbrella headers. Do NOT lengthen
`../../../` chains — new code includes `<module>/<header>` or basename via its owning target.
