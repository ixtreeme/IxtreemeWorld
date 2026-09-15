# Phase-3C workstation validation procedure

Headless CI cannot execute GPU work (no window/swapchain); run this on a
Vulkan workstation with validation layers installed. Extends
`phase3b-validation.md` — run that first, then this.

## 1. Build + smoke

```text
cmake --build Client\build --config Debug
ctest -C Debug -R IXRHISmoke        # expect 100% pass (incl. frame/tracker/result tests)
```

## 2. Minimal GPU frame smoke (§82)

No dedicated harness exists headless; the editor IS the harness:

1. Boot the editor (windowed, non-zero size) — first `BeginFrame` acquires.
2. Confirm steady rendering (scene view + composite + UI present).
3. Close normally — clean shutdown order, no validation errors on teardown.

## 3. Resize stress (§84)

Rapid resize / minimize / restore / maximize, 20–100 cycles. Check:

- 0 validation errors (watch acquire/present/reset/fence/semaphore ordering,
  swapchain image layouts, framebuffer lifetime, query reset/read timing,
  swapchain destruction order).
- `SwapchainRecreated` orchestration runs (pipelines/offscreen re-created;
  scene/game views re-registered without stale images).
- Minimized window: frames Skip without spinning (editor stays responsive).
- No descriptor/command-pool/fence/semaphore/query-pool leaks
  (`RegisteredTextureCount` stable; GPU memory flat).
- No deadlock (fence waits are per-slot + per-image adopted, as before).

## 4. Long-run frame test (§83, recommended)

10,000+ frames with normal render + editor UI + offscreen scene + static
meshes. Check: no descriptor/command-pool/fence/semaphore/query growth, no
memory growth attributable to the frame lifecycle.

## 5. Failure semantics (§85)

- Minimize at startup / zero-size window: Skip path (no crash, no spin).
- Force `VK_ERROR_OUT_OF_DATE_KHR` (e.g. display mode change mid-run):
  `SwapchainRecreated` path, orchestration, continued rendering.
- Do NOT attempt artificial physical device loss.

## 6. Profiler parity (§65)

With a verbose-diagnostics build (`IXTREEME_VERBOSE_DIAGNOSTICS=ON`), trigger
a GPU capture and compare `[GPU-TIME]` output labels/units against pre-3C:
same 23 points, same boundaries, milliseconds. CPU timing fields identical.

## 7. Performance gate (§87/88)

Compare Release frame times on the same scene: the backend issues the same
vk call sequence (fence wait, acquire, reset/begin, submit, present) with no
per-frame heap allocation (preallocated slots/sems/fences/query state; no
per-frame list/fence/semaphore/descriptor construction). Regression >3–5%
attributable to the abstraction requires investigation. Hot-path audit: one
virtual dispatch per frame-lifecycle call (Begin/End), a handful per draw —
unchanged from 3B; no mutexes in the recording path (upload mutex only on
the setup-time staging path).
