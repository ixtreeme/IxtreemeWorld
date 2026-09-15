# Phase-3B workstation validation procedure

Headless CI cannot execute GPU work (no window/swapchain); run this on a
Vulkan workstation with validation layers installed.

## 1. Build + smoke

```text
cmake --build Client\build --config Debug
ctest -C Debug -R IXRHISmoke        # expect 100% pass
```

## 2. Validation run (0 new errors gate)

Launch the editor Debug build with `VK_LAYER_KHRONOS_validation` available
(the engine enables it automatically and logs when missing). Exercise:

1. Boot to empty editor, open a terrain scene.
2. Select meshes/lights/water (selection outlines = IXRHI path).
3. Toggle physics debug overlays (collider/center/contact lines).
4. Open the asset browser over a folder with thumbnails (preview uploads).
5. Resize the viewport repeatedly, including scene/game view splits
   (offscreen Recreate + bridge re-registration each time).
6. Switch render resolution native/fixed (recreateOffscreenScene path).
7. Toggle Play mode (snapshot + second pass + composite + labels paths).

Watch the log for `Vulkan validation error` and for these tags:
`[OFFSCREEN]`, `[EDITOR-SCENE-VIEW]`, `[EDITOR-GAME-VIEW]`,
`[EDITOR-ASSET-PREVIEW]`, `[LOD-ASYNC]`, `[MATBIND-DIAG]`.

## 3. Resize stress (§35)

Repeat step 2.5 twenty times in a row. Check:

- no validation errors,
- no crash, no stale/missing viewport image,
- `RegisteredTextureCount` stable: scene + game + N previews only grow with
  newly viewed thumbnails (add a temporary log in
  `IXVulkanEditorAdapter::SetViewSlot` if counting precisely),
- no growth in GPU memory (RenderDoc or task-manager trend).

## 4. Parity checklist

- Scene/Game view images identical framing and tone map to pre-3B.
- Water refraction present when enabled (snapshot path).
- Labels, outlines, gizmos overlay the scene view.
- Thumbnails appear in the asset browser; project switch clears them
  (no stale images, no leak warnings).

## 5. Performance gate

Compare Release frame times on the same scene before/after: the migrated
paths issue the same vk call sequence (one vcall per draw-state op added).
Regression >3–5% requires investigation.
