# Smart Material implementation log

Started: 2026-10-11. Validated: 2026-10-12. The initial task is audit + actual Phase 1 implementation;
the first milestone also brings forward foundation nodes, layers and instance
asset identity needed to make the architecture usable. The entire eight-phase
framework is **not complete**.

## Starting state and audit

The user's uncommitted graph/editor/bake implementation was retained and extended.
`SMART_MATERIAL_AUDIT.md` records the actual paths and distinguishes CPU execution,
real DXC compilation, simulated editor interaction and missing runtime GPU graph
integration. Baseline Release graph compiler, editor and baking tests passed after
regenerating the stale CMake build with BUILD_TESTING=ON.

## Phase 1 — implemented core milestone

- `MaterialDefinition` is the existing `MaterialGraphDocument`, preserving APIs.
- Stable layer IDs share the graph allocator. Owned regions distinguish managed
  layer nodes, custom nodes and controlled output. Loading validates owners and
  exact managed composition connections.
- Legacy-compatible files remain v2; v1/v2 load; advanced semantics use v3. No
  irreversible migration. Invalid files leave current data intact.
- Existing `GraphInstruction` IR gains deterministic order and a public typed
  parameter/resource layout plus reachable GUID dependency list.
- `MaterialInstance` stores only GUID references and overrides.
  `MaterialInstanceBinding` shares an immutable program, validates/repacks updates
  without compilation and preserves valid data on failure.
- `MaterialGraphIO` provides bounded reads and unique exclusive temporary writes,
  flush and atomic replacement, shared with the production editor.
- New native asset types recognize `.materialgraph`, `.ixsmartmaterial` and
  `.ixmaterialinstance`. Metadata parse/reload and copied-GUID collision handling
  use the existing AssetDatabase. Texture imports expose GUIDs immediately.

Modules: `MaterialGraphDocument.*`, `MaterialLayers.cpp`,
`MaterialGraphCompiler.*`, `MaterialGraphIO.*`, `MaterialInstance.*`,
`AssetDatabase.*`, texture paths in `AssetLibrary.cpp`, render CMake.
New APIs are declared in those headers. Definitions have no GPU handles.

Compatibility: old numeric NodeKind values remain stable; new kinds append.
The optional managed-edit arguments preserve existing calls. Generic editing of
managed inputs/deletion is rejected. Native Unknown enum value is retained.
Published-name renames produce stale override diagnostics instead of remapping
values silently. ABI changes require rebuilding all linked targets.

## Phase 2 — foundation nodes implemented, subgraphs planned

CPU and HLSL now implement Normal Strength/Unpack/Flip Y, Normalize, Cross,
Length, Distance, Sine, Cosine, Fract, Floor, Ceil, Step, Sign, Reflect, Mask
Combine/Levels/Contrast/Invert, Fresnel and Vector2/Vector3 Parameters. Noise's
new seed input preserves earlier pin slots and allows an instance seed override.
Fresnel explicitly consumes world normal and world view direction. Normal map
channels bypass RGB decoding; preset normal samples use linear inputs.

Automated reference-value and invalid-data cases cover every added kind, plus
normal strength 0/.5/1/2, zero vectors, degenerate levels and unsupported mask
operations. DXC checks both DXIL and SPIR-V. Actual GPU numerical parity and
subgraph expansion/assets/cycles are still planned; source compilation is not
GPU evaluation evidence.

## Phase 3 — basic managed stack implemented, advanced layering planned

The production Inspector provides add, duplicate, remove, rename (source node
Inspector), opacity, enable, reorder buttons/drag-and-drop, normal/height blend,
per-channel toggles, scalar mask assignment and mask preview. Graph/stack changes
share complete-document Undo/Redo. Explicit conversion preserves every node,
link, default and channel flag, and Undo restores ownership.

Groups, nested masks, dedicated fill/mask types and additional blend modes are
planned. Masks are currently ordinary reusable graph outputs; there is no second
evaluator and no automatic conversion of arbitrary custom output graphs.
An empty managed stack retains its protected output, serializes and accepts new
layers after the last layer is removed. It renders the existing default surface.

## First Weathered Stone — partial vertical slice

`SmartMaterialPresets.cpp` constructs Stone Base, cavity/AO/noise Dirt and
slope/cavity/noise Moss through the same managed graph. Templates exposes it in
the editor. Stone tiling/normal strength, dirt amount/contrast and moss
amount/scale/slope threshold/seed are published, with optional texture parameters
and neutral color/flat-normal/height defaults.

CPU preview, canonical stack editing, v3 save/load, standalone instance save/load
and HLSL export/compilation are implemented. Different explicitly supplied
geometry contexts affect masks deterministically. **Selected-mesh bake data,
mesh UV baking and live runtime GPU assignment are not implemented.** The UI
labels the missing Geometry Bake and its explicit preview test inputs. The
existing UV-only baker rejects Weathered Stone before creating fake geometry.

## Phases 4–8 — remaining work

| Phase | Status | Required next implementation |
| --- | --- | --- |
| 4 Geometry | Planned | UI-independent mesh baker, UV validation/rasterization/padding, independent curvature/cavity, ray AO/thickness, cache/cancel and preview integration. |
| 5 Generators | Planned | Reusable GUID-backed subgraphs/presets and the six requested generators. The current dirt/moss recipe is ordinary graph content. |
| 6 Assets | Partial | GUID registration, instance files/dependencies implemented; Smart Material Asset Browser categories/loaders, subgraphs, generator assets, recursive dependency invalidation remain. |
| 7 Runtime | Planned | DXC runtime owner, actual reflection, texture/sampler/parameter uploads, variant/pipeline cache, assignment and failure recovery through IXRHI. |
| 8 Optimization | Partial measurement | CPU benchmark available; asynchronous/GPU bake jobs, GPU performance, sample deduplication, folding/CSE and stress/lifetime tests remain. |

There is no external approval blocker or renderer replacement requirement.
Remaining phases are engineering work, not completed capabilities. See
`SMART_MATERIAL_TESTS.md` for actual build/test evidence and
`SMART_MATERIAL_PERFORMANCE.md` for measured values and limits.

## Verification findings corrected during implementation

Integration tests caught missing native graph/instance asset types and texture
imports with empty immediate GUIDs. Those paths were fixed rather than weakening
the tests. Instance tests now guard failed setup and verify GUID rescan, duplicate
metadata safety, dependency persistence and transactional mismatched-instance
failure. Float endpoint comparisons use appropriate tolerance. Advanced channel
semantics remain v3 after conversion to avoid older readers silently ignoring them.

## Verified milestone and next step

Release editor executable and all four material test targets build. The editor
material selection passes 4/4; the separately configured editor-disabled core
passes 2/2. DXC compiles every node and all three presets to DXIL/SPIR-V. The
recorded CPU/CLI measurements are in `SMART_MATERIAL_PERFORMANCE.md`. Standalone
runtime-player rendering, GPU readback and real geometry baking have not been
validated because those material paths are not implemented.

The audit and the already-present pinned node-editor dependency are recorded as
separate commits. The implementation checkpoint includes the user's retained
Material Graph/editor/bake prerequisites because the new core depends on them;
it preserves that work rather than attempting to reconstruct an earlier version.

The next dependency is GUID-backed subgraph expansion into this same typed graph,
with recursive dependency/cycle tests, followed by the independent mesh geometry
baker. Neither requires replacing the renderer. Completing those allows reusable
generator masks and the actual mesh-dependent Weathered Stone CPU/bake workflow;
IXRHI reflection/binding/cache then enables runtime GPU acceptance.
