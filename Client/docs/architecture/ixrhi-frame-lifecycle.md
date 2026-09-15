# IXRHI frame lifecycle (Phase 3C, §147/148)

## AFTER diagram

```text
Application update (input/sim/physics/animation/network — unchanged, §97-99)
        |
rhiDevice.BeginFrame()
        |
IXRHIFrame { Success } ── .info ── IXRHICommandList (owned) + backBuffer
        |
Renderers (migrated: IXRHI list · legacy: synced native cmd via shim)
        |
Offscreen / main targets · editor UI (adapter DrawFrame)
        |
rhiDevice.EndFrame(frame)
        |
IXVulkan Submit (wait acquire-sem, signal present-sem, fence slot)
        |
IXVulkan Present
        |
tracker advance (slot+1, frameNumber+1) · generation on recreate
```

Backend-owned per frame: slot pool/cmd/list/fence/acquire-sem, per-image
present sems + adopted fences, query pool, main targets/backbuffers/depth,
timestamps, CPU timings. No per-frame allocation: everything preallocated and
reused (§49); creation happens at startup, generation rebuild, or first use.

## Ownership graph (§148)

```text
Application
    owns IXRHIDevice (unique_ptr, factory seam)

IXVulkanDevice
    owns backend context objects: frame slots, main swapchain,
    query pool, upload pool, capabilities, tracker state

IXVulkanSwapchain (one object; future windows get their own)
    owns main pass, per-image main targets, per-image depth textures,
    non-owning backbuffer wrappers (borrowed images/views)

IXVulkanFrameSlot (fixed array, kSlots = 2)
    owns command pool/list/sync for its slot

Renderers
    own their IXRHI resources (buffers/textures/pipelines/groups)

Editor bridge/adapter
    bridge holds WEAK registry entries; adapter owns UI registrations
```

## Update vs render separation (§97-101, debt note)

Phase 3C changes GRAPHICS ownership only. The application still runs
simulation-adjacent updates (input, scripts, physics stepping, editor state)
inside the same loop body that renders, and a `Skip` frame skips them — this
is PRE-EXISTING behavior (the old `IsFrameActive` gate had the same shape),
preserved deliberately, not redesigned here. Known debt for the future
MMORPG client: graphics frame, simulation tick, network tick and wall clock
must become independent (`Graphics Frame != Simulation Tick != Network Tick`),
with rendering consuming `ClientWorld` state instead of gating it. No
MMORPG-specific code was introduced; the frame contract takes no Flecs/world
types, so that separation stays possible. Editor frame and game frame are
likewise not fused by this contract.

## Wait audit (§112/113)

| Site | Kind | Verdict |
|---|---|---|
| `IXVulkanDevice::Shutdown` → `WaitIdle` | shutdown | exceptional, keep |
| `RecreateSwapchainNow` → `WaitIdle` (+ legacy internal) | swapchain recreate | exceptional, keep (double idle is redundant but harmless) |
| `OffscreenSceneRenderer::Recreate` → `WaitIdle` | offscreen resize | exceptional, keep (Phase-4 debt: affected-frame wait) |
| `UploadTextureBytes` / `CopyBufferSync` → queue wait | upload parity | preserved Phase-2 behavior; future: staging ring + fence |
| `IXVulkanBufferUpload` fence ( polled, no wait) | async upload | non-blocking, keep |
| Adapter `ReleaseAllPreviewTextures` → loop `WaitIdle` | safety path | exceptional (project switch), keep |
| Legacy `Destroy` → `WaitIdle` | shutdown | exceptional, keep |
| `FinishCaptureAfterSubmit` → queue wait | capture-gated debug | debug-logs builds only, keep |

No per-frame `WaitIdle` anywhere. Upload fence/pools are backend-dedicated;
frame pools/fences never touch uploads (§69 audited).

## Hot-path audit (§88, by inspection)

- `BeginFrame`/`EndFrame`: no heap allocation (preallocated slots/sems;
  vectors sized only on image-count change), two mutex-free paths except the
  setup-only upload mutex (never touched in frame flow), chrono stack only.
- Per draw: same virtual-call granularity as Phase 3B (a handful per
  draw-state op) + pre-existing `shared_ptr` copies in bind-group updates
  (atomic refcount churn, unchanged from 3A — future bind-group batching
  debt, not a 3C regression).
- `EndFrame` token check: one integer compare (kept in Release — negligible).
- Backend `dynamic_cast` on command recording: same as 3B + Debug asserts.
