# Smart Material architecture

## Canonical model and ownership

`MaterialDefinition` aliases `MaterialGraphDocument`; there is one graph, one
`MaterialGraphProgram`, one topological instruction IR and two existing consumers
(CPU and HLSL). The alias preserves public source compatibility. Definitions carry
no Vulkan/IXRHI handles. AssetDatabase `.meta` GUIDs own file identity.

`MaterialLayer` references stable source, opacity, weight and blend nodes, using
the same monotonic ID allocator as nodes and links. Source PBR defaults, opacity,
height blend and channel masks live in the graph nodes, not duplicated layer
values. The layer list owns order, enablement and mask assignment. Reordering
rewires the controlled chain and retains node IDs and surviving link IDs.
Removing the final layer retains an empty managed output so the stack can be
saved and edited again without converting or recreating the definition.

Managed input connections and node deletion are read-only to generic graph APIs.
Custom masks may feed layers and managed outputs may feed custom nodes. Removing
a layer with custom consumers is rejected with a diagnostic. Explicit conversion
clears ownership and layer metadata while preserving graph semantics. The editor
uses its existing complete-document snapshots for Undo/Redo, including conversion.

Version 1 and 2 materialgraph files load without mutation or automatic conversion.
Legacy-compatible custom graphs continue to save as v2. Managed definitions and
graphs using new nodes/channel semantics/seed pins save as v3, with
ownership and layer records; corrupt topology/IDs or unsupported versions fail
transactionally. Unknown future formats are never silently flattened.
Version 4 additionally records typed subgraph declarations and GUID-backed call
interfaces. Version 3 continues to load; graphs without subgraphs retain v2/v3.

## Compiler and instances

`GraphInstruction` remains the shared typed material IR: each input stores an
instruction index/output slot and output numeric types derive from `NodePins`.
Whole-graph validation precedes reachable-node elimination and Kahn ordering.
Node/link array storage order does not change generated HLSL or binding order.
GUID-backed subgraphs are validated/expanded before these steps. Declared ports
become parent bindings/default constants and aliases; all operational instructions
still use the original CPU/HLSL backends. See `SMART_MATERIAL_SUBGRAPHS.md` for
stable interface IDs, instance scoping, bounds, dependencies and editor integration.

`MaterialCompileResult` publishes a deterministic **HLSL source layout**:
numeric parameters ordered by node ID, one aligned float4 per parameter; texture
binding declarations; sorted unique reachable GUID dependencies; context needs.
This is not DXC/SPIR-V reflection and must be checked against actual reflection
when Phase 7 introduces a runtime shader owner.

`MaterialInstance` serializes the definition GUID, optional geometry-bake GUID,
and only numeric/texture overrides. Numeric overrides currently use published
names; node IDs identify layout fields. A published-name rename deliberately
reports stale overrides rather than applying them to a different parameter.
`MaterialInstanceBinding` shares a const program and owns only instance data and
its packed float4 values. It validates changes before replacement and never calls
Compile for dynamic override updates. GPU upload/variant selection is future work.

Texture overrides support `guid:<uuid>`, legacy paths and library IDs. A specified
GUID is authoritative: missing GUIDs cannot accidentally resolve a stale path to
an unrelated texture. CPU texture errors prevent replacing a valid preview/bake.

## Layer semantics

Layer weight is saturated mask * opacity. Color/emissive interpolate in linear
space; roughness, metallic and AO interpolate bounded physical scalar values;
normal uses normalized tangent-space interpolation; height blends use the
existing height-aware weight; opacity interpolates the surface scalar. Channel
bits follow existing PBR input order: color, roughness, metallic, emissive, normal,
AO, height, opacity. Blending does not decide runtime opaque/mask/blend pipeline
selection yet. Specialized additive/multiply modes and layer groups are planned.

## File and module plan

| File/module | Responsibility and dependency |
| --- | --- |
| `MaterialGraphDocument.*` | Canonical data, node schema, v1/v2/v3 serialization; crude_json only. |
| `MaterialLayers.cpp` | Validated layer transactions and controlled topology; same document. |
| `MaterialGraphCompiler.*` | Existing IR/backends plus public parameter/resource layout. |
| `MaterialInstance.*` | Standalone instance JSON, validation and shared program binding. |
| `MaterialGraphIO.*` | Runtime/editor atomic file replacement and bounded reads. |
| `MaterialGraphPanel.*` | Existing ImGui canvas/Inspector/history/preview; consumes these APIs, renders Layer Stack and saves/loads instances. |
| `MaterialGraphAssets.*` | Existing AssetLibrary/MaterialAssetManager bridge and texture cache. |
| `AssetDatabase` / `AssetLibrary` | Recognize graph/instance GUID metadata, prevent duplicate metadata identity, publish texture-import GUIDs immediately; instance save writes native dependencies. |
| `GeometryBaker.*` / `IXGeometryBaker` | UI-independent UV rasterizer, dihedral curvature/cavity, BVH ray AO/thickness, object/world maps, padding, checked cache codec and cancellable worker. |
| `GeometryAssets.*` / `GraphGeometryCache` | Existing Assimp import, native model/cache GUID dependencies, content validation and invalidation; editor thread only. |
| `MaterialSubgraph.*` / `GraphSubgraphCache` | Versioned typed assets, GUID resolution, bounded expansion, cycle checks, caller scopes and file invalidation. |
| `SmartGenerator.*` | Six editable subgraph presets, metadata wrapper, Scalar mask validation and canonical layer insertion; existing IR/backends. |
| Future runtime shader owner | DXC/reflection/cache/IXRHI bindings/lifetime and renderer assignment. |

No independent evaluator, renderer replacement or new third-party dependency is
introduced. Atomic material IO uses unique exclusive temporary files in the
destination directory, flushes file contents, then replaces the destination.

## Real mesh geometry integration

The independent baker owns plain indexed triangles, UVs, normals and an explicit
object-to-world transform. Its output has separate AO/curvature/cavity/thickness,
object/world position/normal, UV-derived world tangent frame, coverage and padding.
It does not evaluate a material or own a graphics resource. The existing shared
evaluator receives real covered surface samples through `EvaluationContext`;
no new evaluator was introduced. See `SMART_MATERIAL_GEOMETRY.md` for algorithms,
cache schema, units, limits and cancellation semantics.

The production editor can select a static source submesh, bake on an independent
worker, inspect the atlas/maps, save/bind a native `.ixgeometrybake`, and save its
GUID in an instance. Loading checks the actual source mesh hash. Dependency
changes invalidate sampling and retain the last valid preview. The cache follows
GUID moves, uses file/metadata stamps for polling, and has an explicit reload for
edits preserving timestamps and sizes. Baking still uses immutable input snapshots.

`BakeMaterialOnGeometry` and native material publication use the same graph IR,
parameters and texture sampler as ordinary UV baking. An object bake retains an
exact cache snapshot and source dependencies. World-dependent rules flatten only
for that mesh atlas and baked placement. Direct GPU graph assignment/reflection,
scene-selected object transforms, terrain and 3D mesh preview remain future work.
