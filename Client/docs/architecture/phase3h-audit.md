# Phase 3H — Vulkan Residue Elimination + IXRHI Completion Audit

Repository-wide audit at 3G HEAD. Method: case-sensitive repo search for
Vulkan includes/types/functions/factories/handles/casts/branches (§4),
per-file classification (§5), followed by minimal enforcement edits.
No feature work (§1); no FPS investigation (§2 — one WaitIdle sweep, no
per-frame sync found, see below).

## Classification result (entire first-party tree, build/ excluded)

- A `Engine/Graphics/Vulkan/*` (~25 files: device, frame, swapchain,
  resources, binding, pipeline, command list, render target/pass, sync,
  conversions, bridge, editor adapter, surface, frame tracker) — backend
  implementation. LEGITIMATE.
- A `libs/platform/VulkanDevice.*` — backend bootstrap: instance, physical/
  logical device, queues, surface, swapchain handle + images + views +
  depth, formats/extents, validation, anisotropy caps, `FindMemoryType`.
  LEGITIMATE infrastructure (§14: no duplicate VkDevice/queue/surface/
  swapchain ownership exists anywhere; §18 graph below).
- B `IXVulkanEditorAdapter` — ImGui Vulkan backend requirement:
  descriptor pool, backend NewFrame/DrawFrame, `ImTextureID` registration
  from IXRHI views/samplers. Isolated behind the Vk-free
  `IEditorTextureProvider`. LEGITIMATE, unchanged.
- C app bootstrap (`EngineApplication.cpp` owns `VulkanDevice device`,
  `ixvulkan::CreateDevice` factory seam, editor-adapter `Create`):
  platform extent/format/size queries + lifetime. LEGITIMATE bootstrap;
  one boundary fix in 3H (offscreen formats now come from
  `rhiDevice->GetMainSwapchain()`, dropping the app's
  `IXVulkanConversions.h` include).
- D–H generic engine — ZERO hits after 3H (see proofs). One fix in 3H:
  `RuntimeSession` already device-free since 3G; no new findings.
- I dead code deleted in 3H: Cube shader source + compiled SPIR-V + build
  rules (no code/test references); stale `IXVulkanDevice.h` duplicate
  `#pragma once`.
- J third-party (`miniaudio.h`, ImGui backends, loader/headers): untouched,
  excluded from the metric (§38).
- K tests: `IXRHISmoke` + `IXRHIConversionsTest` assert the contract
  (incl. IXRHI-header purity); stale `IwSelfTest` source retained but
  unwired — NOT a gate (§44, see below).

False positives triaged, not counted: `Vk*` in prose comments
(`Core.h`, `EditorGraphicsBridge.h`, `NativeWindow*.h`,
`EditorImGui.h`, three `StaticMeshRenderer.cpp` history notes),
`VK_KHR_*_surface` extension-name strings in platform code (data, not
types), `VkExtent2D` as a plain width/height pair in app locals,
`ScriptBackendType`/`Simd Backend` (unrelated enums), `VkHandleBits`
diag helper.

## Zero-Vulkan proofs (§6–13)

- §6 IXRHI headers: zero non-comment Vulkan tokens (verified by filtered
  search); `IXRHISmoke` TUs include every IXRHI header without Vulkan
  headers first.
- §7 renderers (`Static/Skinned/Terrain/RmlUi/SelectionOutline/
  WorldLabel/Offscreen` headers): include only IXRHI + domain headers;
  zero vulkan/IXVulkan includes in all render lib sources.
- §8 generic editor (`EditorImGui.cpp/.h`, panels, `Editor/` tree):
  zero Vulkan tokens outside one bridge comment.
- §10–12 runtime/core/world: zero hits (`Core.h` is comment-only).
- §13 NativeWindow: exposes HWND/`ANativeWindow*` + extension-name
  string only; no `VkInstance`/`VkSurfaceKHR`; surface creation lives in
  `IXVulkanSurface`.

## Ownership graph (§18) — single authority each

- `VkInstance`/`VkDevice`/queues/debug messenger → `VulkanDevice`
  (bootstrap), borrowed by `IXVulkanDevice` (`m_loop`, non-owning).
- Swapchain handle/images/views/depth/formats → `VulkanDevice`;
  passes/targets/backbuffers → `IXVulkanSwapchain`; frames/slots/
  fences/semaphores/query pool/upload pool → `IXVulkanDevice`.
- `ResolveRenderPass(null)` now uses the backend's own swapchain pass
  (3F); no mirror. No second owner of any object above exists.

## VulkanDevice audit (§14) outcome — classify, don't move

Nearly every member is live backend infrastructure called by IXVulkan*
(frame authority, swapchain adoption, capabilities, raisings). Per §16
(no moves for aesthetics in an audit phase) the class STAYS where it is;
the long-term `IXVulkanContext` absorption remains documented future
work. Deleted as dead in 3F: all frame shims + legacy `m_swapchainDirty`.

## Remaining checks (§25–42)

- §25 frame mirrors: zero hits tree-wide (re-verified post-3F).
- §26/27 queries/pools: `VkQueryPool` and command/descriptor pools exist
  only in the backend (frame slots, upload, binding, adapter).
- §28/29 descriptor/resource helpers: backend binding impl + adapter
  pool are legitimate; renderer-side helpers died with their renderers
  (last ones in 3F). `FindMemoryType` stays public as the documented VMA
  insertion point, backend-used only.
- §30 public surface: backend helpers are marked backend-internal in
  comments and called only from `ixvulkan/*`; privatizing across 8
  classes buys nothing — kept, documented.
- §31/32 handles: no `GetVk*`/`GetNative*` APIs exist; native resolution
  only via backend-private `IXVulkanBridge` for the editor adapter.
- §33 casts: all 33 `dynamic_cast<IXVulkan*>` sites are inside
  `Engine/Graphics/Vulkan`; zero in generic code.
- §34 branches: no graphics-backend identity branching (only script/SIMD
  enums and third-party audio).
- §2 WaitIdle sweep: setup/teardown/event-driven sites only
  (recreates, destroys, body-set changes, uploads, shutdown, debug
  capture) — NO per-frame synchronization found. Not fixed (nothing to
  fix), FPS work stays scheduled post-3H.
- §35/36 CMake: `IXEngineRender` no longer links `Vulkan::Vulkan`/`vulkan`
  or `IXEngineVulkan` (headers need neither; Vulkan reaches it
  transitively via `IXEnginePlatform` where `VulkanDevice.h` requires
  it). Verified by full build. `IXEngineIXRHI` stays header-only and
  Vulkan-free; `IXEngineVulkan` links Vulkan; app links the backend
  directly for factory/adapter.
- §37 leak test: renderer headers include IXRHI/domain headers only
  (listed in this report); domain headers (`MapEditorTypes`,
  `WorldCamera`, `SceneManager`, `InputEvent`) are Vulkan-free.
- §39/40 shaders: HLSL → DXC → SPIR-V flow preserved; renderers load
  `.spv` as data through `IXRHIShaderDesc`. No DXIL work in 3H.
- §41/42 readiness recheck: no new IXRHI primitives in 3H → no new
  findings; prior 3D/3E/3F mappings stand. No blockers identified.
- §43 Cube: source, SPIR-V and build rules deleted (unused); README
  corrected.
- §44/45 selftest: source retained (useful gates), still unwired to the
  build graph and still stale — NOT a gate. Current gates:
  `IXRHISmoke` + `ctest`.

## 3H edits (all verified by full build)

1. `EngineApplication.cpp`: offscreen formats via `GetMainSwapchain()`;
   dropped `IXVulkanConversions.h` include.
2. `libs/render/CMakeLists.txt`: dropped `Vulkan::Vulkan`/`vulkan` and
   private `IXEngineVulkan` links (+ stale comment).
3. Deleted Cube shader source/SPIR-V/build rules; README updates
   (renderer boundary doc + `IXVulkan.h` + `Core.h` stale notes).
4. Docs: this report; status-doc title/follow-ups.

## Verification

- Full Debug build: green, zero new warnings (single pre-existing C4189
  in `TerrainRenderer.cpp`).
- `IXEngineVulkan` Release: green. `IXRHISmoke`: 0 failures.
  `ctest -C Debug`: pass.
- Generic-tree Vulkan grep: only the classified legitimate/comment/
  third-party hits above.
