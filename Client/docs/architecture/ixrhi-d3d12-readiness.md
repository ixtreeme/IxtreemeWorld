# IXRHI D3D12 readiness review (Phase 3C, §102-103; Phase-3D compute/buffer update below)

No D3D12 implementation. Per-concept mapping feasibility for a future backend.
Verdict: no Vulkan-shaped contract found that would force a redesign; two
small notes below. No speculative changes made.

## Natural mappings

- `IXRHIDevice` — `ID3D12Device` + queues. Factory seam (`ixvulkan::CreateDevice`)
  is where `CreateDeviceD3D12` would land; renderers take `IXRHIDevice&`.
- `IXRHISwapchain` — DXGI swapchain; `Generation()` maps to buffer-count/
  resize handling; `RequestResize` maps to `ResizeBuffers`. Object (not
  global) already — multi-window safe.
- `IXRHIFrame` / `BeginFrame` / `EndFrame` — allocator + list + fence per
  slot; `SwapchainRecreated` maps to `ResizeBuffers` flow; `Skip`/`DeviceLost`
  (`DXGI_ERROR_DEVICE_REMOVED`) map directly.
- `IXRHICommandList` — direct list; `TransitionTexture`/`CopyTexture` map to
  barriers/copies (our explicit from/to matches D3D12 barrier model BETTER
  than Vulkan's layout model — good).
- `IXRHIRenderTarget` (load/store/clear) — RTV/DSV + `BeginRenderPass`-style
  inheritance or `OMSetRenderTargets`; no dynamic-rendering assumption.
- `IXRHIBuffer` (`Write` for upload-visible) — upload heap; device-local +
  initial data maps to default heap + upload (backend already stages).
- `IXRHITexture`/`IXRHISampler` — committed resources + static samplers.
- `IXRHIBindGroup` (explicit set/slot updates) — descriptor tables copied
  from a CPU-visible heap; matches the update-then-bind flow (not bindless).
- `IXRHIPipeline` (vertex layout, raster, depth, blend, push ranges) —
  PSO desc + root signature; push ranges map to root constants.
- `IXRHIFence`/`IXRHISemaphore` — fence events / shared fences. Frame flow
  itself needs no app-visible semaphores (kept internal — correct for D3D12).
- Query/timestamps — `D3D12_QUERY_TYPE_TIMESTAMP` + resolve; ns conversion
  stays backend-side. Capture-on-request has no D3D12 friction.
- Resource transitions — engine-level `(from, to)` states are API-neutral.

## Notes (no action)

1. `IXRHIImageLayout` names (`ShaderReadOnly`, `ColorAttachment`) read
   Vulkan-flavored but denote universal states; D3D12 maps them to
   `PIXEL_SHADER_RESOURCE` / `RENDER_TARGET` behind the backend. Kept:
   renaming would churn every caller for zero functional gain.
2. `IXRHIFormat` covers what the engine uses; D3D12-only formats get added
   alongside `ToVkFormat`-style mapping when a backend lands.
3. `NativeWindowDesc` currently models Win32/Android handles. D3D12 needs
   `HWND` only — already present. No change.

## Forbidden (verified absent)

No `device.Backend() == Vulkan` branching exists anywhere (no `Backend()`
API at all); all feature paths use `IXRHICapabilities` (§116).

## Phase-3D additions: compute + buffer barriers (no D3D12 implementation)

- `IXRHIComputePipelineDesc` (shader + layouts + push ranges) — compute PSO
  + root signature; `SetComputePipeline`/`Dispatch` map to
  `SetPipelineState`/`Dispatch` on the same direct list (no async queue —
  parity, so no cross-queue fence design needed yet).
- `IXRHIBufferState` (Undefined/ShaderRead/ShaderWrite/VertexRead/
  IndexRead/UniformRead/TransferSrc/TransferDst) — only the states the
  skinned workload uses. D3D12 mapping is natural, classic or enhanced:
  ShaderRead → NON_PIXEL_SHADER_RESOURCE (+PIXEL), ShaderWrite →
  UNORDERED_ACCESS, VertexRead → VERTEX_AND_CONSTANT_BUFFER, IndexRead →
  INDEX_BUFFER, UniformRead → CONSTANT_BUFFER (or VERTEX_AND_CONSTANT),
  TransferSrc → COPY_SOURCE, TransferDst → COPY_DEST, Undefined → no-access
  top state. `TransitionBuffer(from, to)` carries both sides, matching the
  D3D12 barrier model (same argument as the texture transitions in 3C).
- `CopyBuffer` — `CopyBufferRegion` (same-queue ordering = recording order).
- `ExecuteAndWait` (one-time setup/readback submit + fence wait) — direct
  list + fence; must not be called from inside a recording list (documented
  on the API).
- `ClearDepth` with rect — `ClearDepthStencilView` with a rect.
- Verdict: no Vulkan-shaped contract added; the (from, to) buffer states
  are, if anything, closer to D3D12 than to Vulkan's stage/access pairing.
