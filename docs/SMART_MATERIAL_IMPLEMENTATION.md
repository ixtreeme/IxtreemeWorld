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

## Phase 2 — foundation nodes and subgraphs implemented; GPU parity pending

CPU and HLSL now implement Normal Strength/Unpack/Flip Y, Normalize, Cross,
Length, Distance, Sine, Cosine, Fract, Floor, Ceil, Step, Sign, Reflect, Mask
Combine/Levels/Contrast/Invert, Fresnel and Vector2/Vector3 Parameters. Noise's
new seed input preserves earlier pin slots and allows an instance seed override.
Fresnel explicitly consumes world normal and world view direction. Normal map
channels bypass RGB decoding; preset normal samples use linear inputs.

Automated reference-value and invalid-data cases cover every added kind, plus
normal strength 0/.5/1/2, zero vectors, degenerate levels and unsupported mask
operations. DXC checks both DXIL and SPIR-V. Actual GPU numerical parity
remains planned; source compilation is not GPU evaluation
evidence. Subgraph expansion/assets/cycles were implemented in the next milestone
described below.

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
geometry contexts affect masks deterministically. The next geometry milestone
adds selected static submesh bake data, atlas preview and object material export.
Analytic sphere/plane previews still label their explicit test inputs when those
are used. Ordinary UV export continues rejecting Weathered Stone; the dedicated
mesh path requires a verified Geometry Bake. Live runtime GPU graph assignment
remains planned.

## Phases 4–8 — remaining work

| Phase | Status | Required next implementation |
| --- | --- | --- |
| 4 Geometry | CPU/editor reference implemented | Static submesh atlas, maps, checked cache/GUID invalidation, async/cancel and material integration implemented. Scene-object binding, terrain, skinned baking, 3D mesh preview and broader stress profiling remain. |
| 5 Generators | Planned | The reusable subgraph mechanism and real geometry inputs are implemented; six reusable generator presets remain. The current dirt/moss recipe is ordinary graph content. |
| 6 Assets | Partial | Graph/instance/subgraph/geometry GUID registration, files and native dependencies implemented; geometry content hashes/invalidation implemented. Dedicated browser categories, generator assets and runtime invalidation remain. |
| 7 Runtime | Planned | DXC runtime owner, actual reflection, texture/sampler/parameter uploads, variant/pipeline cache, assignment and failure recovery through IXRHI. |
| 8 Optimization | Partial | CPU benchmarks and cancellable async geometry jobs implemented. Material preview/export still synchronous; GPU baking/performance, sample deduplication, folding/CSE and broader stress/lifetime tests remain. |

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

## First milestone validation and current next step

At the first milestone, the Release editor executable and all four material test targets built. The editor
material selection passes 4/4; the separately configured editor-disabled core
passes 2/2. DXC compiles every node and all three presets to DXIL/SPIR-V. The
recorded CPU/CLI measurements are in `SMART_MATERIAL_PERFORMANCE.md`. Standalone
runtime-player rendering, GPU readback and real geometry baking have not been
validated because those material paths are not implemented.

The audit and the already-present pinned node-editor dependency are recorded as
separate commits. The implementation checkpoint includes the user's retained
Material Graph/editor/bake prerequisites because the new core depends on them;
it preserves that work rather than attempting to reconstruct an earlier version.

The subgraph dependency is now implemented. The next dependency is the independent
mesh geometry baker; it does not require replacing the renderer. Geometry data then
allows generator presets and the actual mesh-dependent Weathered Stone CPU/bake
workflow. IXRHI reflection/binding/cache enables runtime GPU acceptance afterward.

## Milestone 2 — reusable subgraph assets

Implemented `MaterialSubgraph`, input/output/call nodes, graph v4, file wrapper v1,
typed stable-ID interfaces, scoped published parameters and transactional expansion
into the existing compiler. Cycles, missing assets, mismatched schemas and expansion
limits report errors while preserving valid state. Ordinary-node provenance is
available in the compile result and generated HLSL.

Native AssetDatabase recognizes subgraphs. The existing editor creates/opens/saves
them, binds and refreshes interfaces, and validates connected port changes. It
exposes the compiled parameter layout, including nested scopes. Native GUID cache
polling refreshes changed/moved files, preserves valid previews on malformed files
and recovers when fixed. Definition saves record native dependencies. UV publishing
keeps both the authored graph and an effective self-contained snapshot with scoped
overrides. This does not implement runtime GPU or geometry baking.

Modules: new `MaterialSubgraph.*`, document/compiler, graph assets/cache/panel,
native AssetDatabase type, render CMake and tests. Public source-compatible Compile
adds an optional resolver argument. Existing enum values and v1/v2/v3 formats remain.
New API/schema versions require rebuilding linked consumers. Source defaults on
calls are snapshots; refresh is explicit. Mtime/size polling has a manual reload
path for edits that preserve both fields. Dedicated subgraph browser categories,
content hashes and asynchronous expansion are still planned.

`MaterialSubgraphTest` exercises real production expansion and actual DXC. Editor
tests exposed a selected-output restoration bug after recreating the node-editor
context; selection is now restored once nodes exist. Real files exercise native
GUID moves, invalidation, failure recovery, standalone preview and instance reload.
The baking test verifies effective source override values and native material output.
Asset tests also exposed delayed native GUID path updates after texture moves.
Successful AssetLibrary moves/renames now update the native database immediately;
GUID sampling without a library pointer follows those paths and never falls back
to a stale hint. The full 256-entry dependency cache can be replaced by a new
definition without manual clearing. Validated interfaces are cached per GUID in
the compiler, with before/after measurements in the performance document.
Final builds/results are recorded in `SMART_MATERIAL_TESTS.md` after verification.

## Milestone 3 — real static mesh geometry baker

`IXGeometryBaker` is an independent C++20 library, available without editor or
renderer dependencies. It validates/rasterizes the actual indexed mesh UV atlas,
welds exact-position seams for topology, calculates signed dihedral curvature and
separate concavity, ray-tests AO/thickness through a BVH, transforms object/world
position/normal and derives the real UV tangent frame. Deterministic padding keeps
the original covered UV. Invalid geometry, settings, overlapping sampled texels,
cache corruption and cancellation preserve previous valid output.

`GeometryAssets` imports existing Assimp submeshes and owns native model/cache GUID
binding, content hash validation and file/metadata invalidation. The editor starts
cancellable worker jobs, displays progress, opens/saves caches and binds them into
instances. Actual AO/Curvature/Cavity nodes receive these maps through the existing
evaluator. The preview displays the mesh UV atlas and raw geometry channels; it is
not a 3D mesh renderer. Source and cache moves keep identity; changed source content
requires rebaking and keeps the prior preview image with a diagnostic.

Weathered Stone can now publish an object-specific native material through the
existing asset pipeline, with real PNG/ORM images, a checked geometry snapshot and
dependency metadata. Source placement and UVs are baked into these images. The
runtime shader/reflection path and six reusable smart generators are still planned.
No extra dependency was introduced; C++ standard threads power the geometry job.

Reference fixtures and real OBJ files verify geometry, corruption/cancel recovery,
cache moves/staleness, editor job completion, instance reload and material output.
The editor sequence found a stale Selected Output mode after switching from a
subgraph to an ordinary material; Load now resets that unavailable selection.
The cache/algorithm schema and current limitations are in `SMART_MATERIAL_GEOMETRY.md`.
